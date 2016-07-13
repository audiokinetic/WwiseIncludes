/*******************************************************************************
The content of this file includes portions of the AUDIOKINETIC Wwise Technology
released in source code form as part of the SDK installer package.

Commercial License Usage

Licensees holding valid commercial licenses to the AUDIOKINETIC Wwise Technology
may use this file in accordance with the end user license agreement provided 
with the software or, alternatively, in accordance with the terms contained in a
written agreement between you and Audiokinetic Inc.

Apache License Usage

Alternatively, this file may be used under the Apache License, Version 2.0 (the 
"Apache License"); you may not use this file except in compliance with the 
Apache License. You may obtain a copy of the Apache License at 
http://www.apache.org/licenses/LICENSE-2.0.

Unless required by applicable law or agreed to in writing, software distributed
under the Apache License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES
OR CONDITIONS OF ANY KIND, either express or implied. See the Apache License for
the specific language governing permissions and limitations under the License.

  Version: 2016.1  Build: 5775
  Copyright (c) 2016 Audiokinetic Inc.
*******************************************************************************/

/*
 * Emulates a subset of the Win32 threading API as a layer on top of WinRT threadpools.
 *
 * Supported features:
 *
 *    - CreateThread (returns a standard Win32 handle which can be waited on, then closed)
 *    - CREATE_SUSPENDED and ResumeThread
 *    - Partial support for SetThreadPriority (see below)
 *    - Sleep
 *    - Thread local storage (TlsAlloc, TlsFree, TlsGetValue, TlsSetValue)
 *
 * Differences from Win32:
 *
 *    - No ExitThread or TerminateThread (just return from the thread function to exit)
 *    - No SuspendThread, so ResumeThread is only useful in combination with CREATE_SUSPENDED
 *    - SetThreadPriority is only available while a thread is in CREATE_SUSPENDED state
 *    - SetThreadPriority only supports three priority levels (negative, zero, or positive)
 *    - TLS requires use of this CreateThread (leaks memory if called from other threadpool tasks)
 *    - No thread identifier APIs (GetThreadId, GetCurrentThreadId, OpenThread)
 *    - No affinity APIs
 *    - No GetExitCodeThread
 *    - Failure cases return error codes but do not always call SetLastError
 */

#pragma once

namespace AK
{
	namespace ThreadEmulation
	{
		HANDLE WINAPI CreateThread(__in LPTHREAD_START_ROUTINE lpStartAddress, __in_opt LPVOID lpParameter, __in DWORD dwCreationFlags);
		DWORD WINAPI ResumeThread(__in HANDLE hThread);
		BOOL WINAPI SetThreadPriority(__in HANDLE hThread, __in int nPriority);
	
		VOID WINAPI Sleep(__in DWORD dwMilliseconds);
		VOID WINAPI SleepEx(__in DWORD dwMilliseconds, BOOL in_bAlertable );
	}
}

#ifdef AK_IMPLEMENT_THREAD_EMULATION

#include <assert.h>
#include <vector>
#include <set>
#include <map>
#include <mutex>

namespace AK
{
	namespace ThreadEmulation
	{
		// Stored data for CREATE_SUSPENDED and ResumeThread.
		struct PendingThreadInfo
		{
			LPTHREAD_START_ROUTINE lpStartAddress;
			LPVOID lpParameter;
			HANDLE completionEvent;
			int nPriority;
		};

		static std::map<HANDLE, PendingThreadInfo> pendingThreads;
		static std::mutex pendingThreadsLock;


		// Converts a Win32 thread priority to WinRT format.
		static Windows::System::Threading::WorkItemPriority GetWorkItemPriority(int nPriority)
		{
			if (nPriority < 0)
				return Windows::System::Threading::WorkItemPriority::Low;
			else if (nPriority > 0)
				return Windows::System::Threading::WorkItemPriority::High;
			else
				return Windows::System::Threading::WorkItemPriority::Normal;
		}


		// Helper shared between CreateThread and ResumeThread.
		static void StartThread(LPTHREAD_START_ROUTINE lpStartAddress, LPVOID lpParameter, HANDLE completionEvent, int nPriority)
		{
			auto workItemHandler = ref new Windows::System::Threading::WorkItemHandler([=](Windows::Foundation::IAsyncAction^)
			{
				// Run the user callback.
				lpStartAddress(lpParameter);

				// Signal that the thread has completed.
				SetEvent(completionEvent);
				CloseHandle(completionEvent);

			}, Platform::CallbackContext::Any);

			// Start the threadpool task.
			auto workItem = Windows::System::Threading::ThreadPool::RunAsync(workItemHandler, GetWorkItemPriority(nPriority), Windows::System::Threading::WorkItemOptions::TimeSliced );
		
	//		workItem->Start();
		}


		HANDLE WINAPI CreateThread(__in LPTHREAD_START_ROUTINE lpStartAddress, __in_opt LPVOID lpParameter, __in DWORD dwCreationFlags)
		{
			// Validate parameters.
			assert((dwCreationFlags & ~CREATE_SUSPENDED) == 0);

			// Create a handle that will be signalled when the thread has completed.
			HANDLE threadHandle = CreateEventEx(nullptr, nullptr, CREATE_EVENT_MANUAL_RESET, STANDARD_RIGHTS_ALL|EVENT_MODIFY_STATE);

			if (!threadHandle)
				return nullptr;

			// Make a copy of the handle for internal use. This is necessary because
			// the caller is responsible for closing the handle returned by CreateThread,
			// and they may do that before or after the thread has finished running.
			HANDLE completionEvent;
		
			if (!DuplicateHandle(GetCurrentProcess(), threadHandle, GetCurrentProcess(), &completionEvent, 0, false, DUPLICATE_SAME_ACCESS))
			{
				CloseHandle(threadHandle);
				return nullptr;
			}

			try
			{
				if (dwCreationFlags & CREATE_SUSPENDED)
				{
					// Store info about a suspended thread.
					PendingThreadInfo info;

					info.lpStartAddress = lpStartAddress;
					info.lpParameter = lpParameter;
					info.completionEvent = completionEvent;
					info.nPriority = 0;

					std::lock_guard<std::mutex> lock(pendingThreadsLock);

					pendingThreads[threadHandle] = info;
				}
				else
				{
					// Start the thread immediately.
					StartThread(lpStartAddress, lpParameter, completionEvent, 0);
				}
	
				return threadHandle;
			}
			catch (...)
			{
				// Clean up if thread creation fails.
				CloseHandle(threadHandle);
				CloseHandle(completionEvent);

				return nullptr;
			}
		}


		DWORD WINAPI ResumeThread(__in HANDLE hThread)
		{
			std::lock_guard<std::mutex> lock(pendingThreadsLock);

			// Look up the requested thread.
			auto threadInfo = pendingThreads.find(hThread);

			if (threadInfo == pendingThreads.end())
			{
				// Can only resume threads while they are in CREATE_SUSPENDED state.
				assert(false);
				return (DWORD)-1;
			}

			PendingThreadInfo& info = threadInfo->second;

			// Start the thread.
			try
			{
				StartThread(info.lpStartAddress, info.lpParameter, info.completionEvent, info.nPriority);
			}
			catch (...)
			{
				return (DWORD)-1;
			}

			// Remove this thread from the pending list.
			pendingThreads.erase(threadInfo);

			return 0;
		}


		BOOL WINAPI SetThreadPriority(__in HANDLE hThread, __in int nPriority)
		{
			std::lock_guard<std::mutex> lock(pendingThreadsLock);

			// Look up the requested thread.
			auto threadInfo = pendingThreads.find(hThread);

			if (threadInfo == pendingThreads.end())
			{
				// Can only set priority on threads while they are in CREATE_SUSPENDED state.
				assert(false);
				return false;
			}

			// Store the new priority.
			threadInfo->second.nPriority = nPriority;

			return true;
		}

		VOID WINAPI Sleep(__in DWORD dwMilliseconds )
		{
			SleepEx( dwMilliseconds, FALSE );
		}

		VOID WINAPI SleepEx(__in DWORD dwMilliseconds, BOOL in_bAlertable)
		{
			static HANDLE singletonEvent = nullptr;

			HANDLE sleepEvent = singletonEvent;

			// Demand create the event.
			if (!sleepEvent)
			{
				sleepEvent = CreateEventEx(nullptr, nullptr, CREATE_EVENT_MANUAL_RESET, STANDARD_RIGHTS_ALL|EVENT_MODIFY_STATE);

				if (!sleepEvent)
					return;

				HANDLE previousEvent = InterlockedCompareExchangePointerRelease(&singletonEvent, sleepEvent, nullptr);
			
				if (previousEvent)
				{
					// Back out if multiple threads try to demand create at the same time.
					CloseHandle(sleepEvent);
					sleepEvent = previousEvent;
				}
			}

			// Emulate sleep by waiting with timeout on an event that is never signalled.
			DWORD dwResult = WaitForSingleObjectEx(sleepEvent, dwMilliseconds, in_bAlertable);
			assert( dwResult != -1 );
		}
	}
}

#endif