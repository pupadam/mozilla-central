/* -*- Mode: C++; tab-width: 20; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is Mozilla Corporation code.
 *
 * The Initial Developer of the Original Code is Mozilla Foundation.
 * Portions created by the Initial Developer are Copyright (C) 2009
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Bas Schouten <bschouten@mozilla.com>
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#include "ReadbackManagerD3D10.h"
#include "ReadbackProcessor.h"

#include "nsIThread.h"
#include "nsThreadUtils.h"
#include "gfxImageSurface.h"

namespace mozilla {
namespace layers {

// Structure that contains the information required to execute a readback task,
// the only member accessed off the main thread here is mReadbackTexture. Since
// mLayer may be released only on the main thread this object should always be
// destroyed on the main thread!
struct ReadbackTask {
  // The texture that we copied the contents of the thebeslayer to.
  nsRefPtr<ID3D10Texture2D> mReadbackTexture;
  // This exists purely to keep the ReadbackLayer alive for the lifetime of
  // mUpdate. Note that this addref and release should occur -solely- on the
  // main thread.
  nsRefPtr<ReadbackLayer> mLayer;
  ReadbackProcessor::Update mUpdate;
  // The origin in ThebesLayer coordinates of mReadbackTexture.
  gfxPoint mOrigin;
  // mLayer->GetBackgroundOffset() when the task is created.  We have
  // to save this in the ReadbackTask because it might change before
  // the update is delivered to the readback sink.
  nsIntPoint mBackgroundOffset;
};

// This class is created and dispatched from the Readback thread but it must be
// destroyed by the main thread.
class ReadbackResultWriter : public nsIRunnable
{
  NS_DECL_ISUPPORTS
public:
  ReadbackResultWriter(ReadbackTask *aTask) : mTask(aTask) {}

  NS_IMETHODIMP Run()
  {
    ReadbackProcessor::Update *update = &mTask->mUpdate;

    if (!update->mLayer->GetSink()) {
      // This can happen when a plugin is destroyed.
      return NS_OK;
    }

    nsIntPoint offset = mTask->mBackgroundOffset;

    D3D10_TEXTURE2D_DESC desc;
    mTask->mReadbackTexture->GetDesc(&desc);

    D3D10_MAPPED_TEXTURE2D mappedTex;
    // We know this map will immediately succeed, as we've already mapped this
    // copied data on our task thread.
    HRESULT hr = mTask->mReadbackTexture->Map(0, D3D10_MAP_READ, 0, &mappedTex);

    if (FAILED(hr)) {
      // If this fails we're never going to get our ThebesLayer content.
      update->mLayer->GetSink()->SetUnknown(update->mSequenceCounter);
      return NS_OK;
    }

    nsRefPtr<gfxImageSurface> sourceSurface =
      new gfxImageSurface((unsigned char*)mappedTex.pData,
                          gfxIntSize(desc.Width, desc.Height),
                          mappedTex.RowPitch,
                          gfxASurface::ImageFormatRGB24);

    nsRefPtr<gfxContext> ctx =
      update->mLayer->GetSink()->BeginUpdate(update->mUpdateRect + offset,
                                             update->mSequenceCounter);

    if (ctx) {
      ctx->Translate(gfxPoint(offset.x, offset.y));
      ctx->SetSource(sourceSurface, gfxPoint(mTask->mOrigin.x,
                                             mTask->mOrigin.y));
      ctx->Paint();

      update->mLayer->GetSink()->EndUpdate(ctx, update->mUpdateRect + offset);
    }

    mTask->mReadbackTexture->Unmap(0);

    return NS_OK;
  }

private:
  nsAutoPtr<ReadbackTask> mTask;
};

NS_IMPL_THREADSAFE_ISUPPORTS1(ReadbackResultWriter, nsIRunnable)

DWORD WINAPI StartTaskThread(void *aManager)
{
  static_cast<ReadbackManagerD3D10*>(aManager)->ProcessTasks();

  return 0;
}

ReadbackManagerD3D10::ReadbackManagerD3D10()
  : mRefCnt(0)
{
  ::InitializeCriticalSection(&mTaskMutex);
  mShutdownEvent = ::CreateEventA(NULL, FALSE, FALSE, NULL);
  mTaskSemaphore = ::CreateSemaphoreA(NULL, 0, 1000000, NULL);
  mTaskThread = ::CreateThread(NULL, 0, StartTaskThread, this, 0, 0);
}

ReadbackManagerD3D10::~ReadbackManagerD3D10()
{
  ::SetEvent(mShutdownEvent);

  // This shouldn't take longer than 5 seconds, if it does we're going to choose
  // to leak the thread and its synchronisation in favor of crashing or freezing
  DWORD result = ::WaitForSingleObject(mTaskThread, 5000);
  if (result != WAIT_TIMEOUT) {
    ::DeleteCriticalSection(&mTaskMutex);
    ::CloseHandle(mShutdownEvent);
    ::CloseHandle(mTaskSemaphore);
    ::CloseHandle(mTaskThread);
  } else {
    NS_RUNTIMEABORT("ReadbackManager: Task thread did not shutdown in 5 seconds.");
  }
}

void
ReadbackManagerD3D10::PostTask(ID3D10Texture2D *aTexture, void *aUpdate, const gfxPoint &aOrigin)
{
  ReadbackTask *task = new ReadbackTask;
  task->mReadbackTexture = aTexture;
  task->mUpdate = *static_cast<ReadbackProcessor::Update*>(aUpdate);
  task->mOrigin = aOrigin;
  task->mLayer = task->mUpdate.mLayer;
  task->mBackgroundOffset = task->mLayer->GetBackgroundLayerOffset();

  ::EnterCriticalSection(&mTaskMutex);
  mPendingReadbackTasks.AppendElement(task);
  ::LeaveCriticalSection(&mTaskMutex);

  ::ReleaseSemaphore(mTaskSemaphore, 1, NULL);
}

HRESULT
ReadbackManagerD3D10::QueryInterface(REFIID riid, void **ppvObject)
{
  if (!ppvObject) {
    return E_POINTER;
  }

  if (riid == IID_IUnknown) {
    *ppvObject = this;
  } else {
    return E_NOINTERFACE;
  }

  return S_OK;
}

ULONG
ReadbackManagerD3D10::AddRef()
{
  NS_ASSERTION(NS_IsMainThread(),
    "ReadbackManagerD3D10 should only be refcounted on main thread.");
  return ++mRefCnt;
}

ULONG
ReadbackManagerD3D10::Release()
{
  NS_ASSERTION(NS_IsMainThread(),
    "ReadbackManagerD3D10 should only be refcounted on main thread.");
  ULONG newRefCnt = --mRefCnt;
  if (!newRefCnt) {
    mRefCnt++;
    delete this;
  }
  return newRefCnt;
}

void
ReadbackManagerD3D10::ProcessTasks()
{
  HANDLE handles[] = { mTaskSemaphore, mShutdownEvent };
  
  while (true) {
    DWORD result = ::WaitForMultipleObjects(2, handles, FALSE, INFINITE);
    if (result != WAIT_OBJECT_0) {
      return;
    }

    ::EnterCriticalSection(&mTaskMutex);
    if (mPendingReadbackTasks.Length() == 0) {
      NS_RUNTIMEABORT("Trying to read from an empty array, bad bad bad");
    }
    ReadbackTask *nextReadbackTask = mPendingReadbackTasks[0].forget();
    mPendingReadbackTasks.RemoveElementAt(0);
    ::LeaveCriticalSection(&mTaskMutex);

    // We want to block here until the texture contents are available, the
    // easiest thing is to simply map and unmap.
    D3D10_MAPPED_TEXTURE2D mappedTex;
    nextReadbackTask->mReadbackTexture->Map(0, D3D10_MAP_READ, 0, &mappedTex);
    nextReadbackTask->mReadbackTexture->Unmap(0);

    // We can only send the update to the sink on the main thread, so post an
    // event there to do so. Ownership of the task is passed from
    // mPendingReadbackTasks to ReadbackResultWriter here.
    nsCOMPtr<nsIThread> thread = do_GetMainThread();
    thread->Dispatch(new ReadbackResultWriter(nextReadbackTask),
                     nsIEventTarget::DISPATCH_NORMAL);
  }
}

}
}
