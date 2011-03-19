/******************************************************************************
Copyright (c) 2010, Google Inc.
All rights reserved.

Redistribution and use in source and binary forms, with or without 
modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice, 
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice,
      this list of conditions and the following disclaimer in the documentation
      and/or other materials provided with the distribution.
    * Neither the name of the <ORGANIZATION> nor the names of its contributors 
    may be used to endorse or promote products derived from this software 
    without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" 
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE 
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE 
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE 
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL 
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR 
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER 
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, 
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE 
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
******************************************************************************/

#include "StdAfx.h"
#include "test_state.h"
#include "results.h"
#include "screen_capture.h"
#include "shared_mem.h"
#include "../wptdriver/util.h"
#include "cximage/ximage.h"
#include <Mmsystem.h>

const DWORD ACTIVITY_TIMEOUT = 2000;
const DWORD ON_LOAD_GRACE_PERIOD = 1000;
const DWORD SCREEN_CAPTURE_INCREMENTS = 20;
const DWORD DATA_COLLECTION_INTERVAL = 100;
const DWORD START_RENDER_MARGIN = 30;

/*-----------------------------------------------------------------------------
-----------------------------------------------------------------------------*/
TestState::TestState(int test_timeout, bool end_on_load, Results& results,
                      ScreenCapture& screen_capture):
  _test_timeout(test_timeout)
  ,_active(false)
  ,_timeout(false)
  ,_next_document(1)
  ,_current_document(0)
  ,_doc_requests(0)
  ,_requests(0)
  ,_doc_bytes_in(0)
  ,_bytes_in(0)
  ,_doc_bytes_out(0)
  ,_bytes_out(0)
  ,_last_bytes_in(0)
  ,_end_on_load(end_on_load)
  ,_results(results)
  ,_screen_capture(screen_capture)
  ,_frame_window(NULL)
  ,_document_window(NULL)
  ,_screen_updated(false)
  ,_render_check_thread(NULL)
  ,_exit(false)
  ,_data_timer(NULL)
  ,_last_data_ms(0)
  ,_last_process_time(0)
  ,_video_capture_count(0) {
  _start.QuadPart = 0;
  _on_load.QuadPart = 0;
  _render_start.QuadPart = 0;
  _first_activity.QuadPart = 0;
  _last_activity.QuadPart = 0;
  _first_byte.QuadPart = 0;
  _last_real_time.QuadPart = 0;
  _last_video_time.QuadPart = 0;
  QueryPerformanceFrequency(&_ms_frequency);
  _ms_frequency.QuadPart = _ms_frequency.QuadPart / 1000;
  _check_render_event = CreateEvent(NULL, FALSE, FALSE, NULL);
  InitializeCriticalSection(&_data_cs);
}

/*-----------------------------------------------------------------------------
-----------------------------------------------------------------------------*/
TestState::~TestState(void) {
  Done();
  DeleteCriticalSection(&_data_cs);
}

/*-----------------------------------------------------------------------------
-----------------------------------------------------------------------------*/
static unsigned __stdcall RenderCheckThread( void* arg ) {
  TestState * test_state = (TestState *)arg;
  if( test_state )
    test_state->RenderCheckThread();
    
  return 0;
}

/*-----------------------------------------------------------------------------
-----------------------------------------------------------------------------*/
void __stdcall CollectData(PVOID lpParameter, BOOLEAN TimerOrWaitFired) {
  if( lpParameter )
    ((TestState *)lpParameter)->CollectData();
}

/*-----------------------------------------------------------------------------
-----------------------------------------------------------------------------*/
void TestState::Start() {
  ATLTRACE2(_T("[wpthook] TestState::Start()\n"));
  QueryPerformanceCounter(&_start);
  _results.Reset();
  _timeout = false;
  _active = true;
  _screen_updated = false;
  _current_document = _next_document;
  _next_document++;
  FindBrowserWindow();  // the document window may not be available yet
  _exit = false;
  ResetEvent(_check_render_event);
  _render_check_thread = (HANDLE)_beginthreadex(0, 0, ::RenderCheckThread, 
                                                                   this, 0, 0);
  timeBeginPeriod(1);
  CreateTimerQueueTimer(&_data_timer, NULL, ::CollectData, this, 
        DATA_COLLECTION_INTERVAL, DATA_COLLECTION_INTERVAL, WT_EXECUTEDEFAULT);
  CollectData();
}

/*-----------------------------------------------------------------------------
-----------------------------------------------------------------------------*/
void TestState::ActivityDetected() {
  if (_active) {
    QueryPerformanceCounter(&_last_activity);
    if (!_first_activity.QuadPart)
      _first_activity.QuadPart = _last_activity.QuadPart;
  }
}

/*-----------------------------------------------------------------------------
-----------------------------------------------------------------------------*/
void TestState::OnNavigate() {
  if (_active) {
    ATLTRACE2(_T("[wpthook] TestState::OnNavigate()\n"));
    FindBrowserWindow();
    GrabVideoFrame(true);
    _on_load.QuadPart = 0;
    if (!_current_document) {
      _current_document = _next_document;
      _next_document++;
    }
  }
}

/*-----------------------------------------------------------------------------
-----------------------------------------------------------------------------*/
void TestState::OnLoad() {
  if (_active) {
    ATLTRACE2(_T("[wpthook] TestState::OnLoad()\n"));
    QueryPerformanceCounter(&_on_load);
    _current_document = 0;
    _screen_capture.Capture(_document_window, 
                                  CapturedImage::DOCUMENT_COMPLETE);
  }
}

/*-----------------------------------------------------------------------------
-----------------------------------------------------------------------------*/
bool TestState::IsDone() {
  bool done = false;

  if (_active){
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    __int64 elapsed_test = 0;
    __int64 elapsed_doc = 0;
    __int64 elapsed_activity = 0;

    // calculate the varous elapsed times
    if (_start.QuadPart && now.QuadPart >= _start.QuadPart)
      elapsed_test = (now.QuadPart - _start.QuadPart) / _ms_frequency.QuadPart;

    if (_on_load.QuadPart && now.QuadPart >= _on_load.QuadPart)
      elapsed_doc = (now.QuadPart - _on_load.QuadPart) 
                    / _ms_frequency.QuadPart;

    if (_last_activity.QuadPart && now.QuadPart >= _last_activity.QuadPart)
      elapsed_activity = (now.QuadPart - _last_activity.QuadPart)
                         / _ms_frequency.QuadPart;

    if (elapsed_test > _test_timeout){
      // the test timed out
      _timeout = true;
      done = true;
    } else if (!_current_document && _end_on_load &&
                elapsed_doc && elapsed_doc > ON_LOAD_GRACE_PERIOD){
      // end 1 second after onLoad regardless of activity
      done = true;
    } else if (!_current_document && !_end_on_load &&
                elapsed_doc && elapsed_doc > ON_LOAD_GRACE_PERIOD &&
                elapsed_activity && elapsed_activity > ACTIVITY_TIMEOUT){
      // the normal mode of waiting for 2 seconds of no network activity after
      // onLoad
      done = true;
    }

    if (done) {
      Done();
    }
  }

  return done;
}

/*-----------------------------------------------------------------------------
-----------------------------------------------------------------------------*/
void TestState::Done(void) {
  if (_active) {
    _screen_capture.Capture(_document_window, CapturedImage::FULLY_LOADED);

    // kill the timer that was collecting periodic data (cpu, video, etc)
    if (_data_timer) {
      DeleteTimerQueueTimer(NULL, _data_timer, NULL);
      _data_timer = NULL;
      timeEndPeriod(1);
    }

    // clean up the background thread that was doing the timer checks
    if (_render_check_thread) {
      _exit = true;
      SetEvent(_check_render_event);
      WaitForSingleObject(_render_check_thread, INFINITE);
      CloseHandle(_render_check_thread);
      _render_check_thread = NULL;
    }
  }
}

/*-----------------------------------------------------------------------------
    Find the browser window that we are going to capture
-----------------------------------------------------------------------------*/
void TestState::FindBrowserWindow(void) {
  DWORD browser_process_id = GetCurrentProcessId();
  if (::FindBrowserWindow(browser_process_id, _frame_window, 
                          _document_window)) {
    ATLTRACE(_T("[wpthook] - Frame Window: %08X, Document Window: %08X\n"), 
                    _frame_window, _document_window);
    if (!_document_window)
      _document_window = _frame_window;
  }
}

/*-----------------------------------------------------------------------------
    Grab a video frame if it is appropriate
-----------------------------------------------------------------------------*/
void TestState::GrabVideoFrame(bool force) {
  if (_active && _document_window && shared_capture_video) {
    if (force || (_screen_updated && _render_start.QuadPart)) {
      // use a falloff on the resolution with which we capture video
      bool grab_video = false;
      LARGE_INTEGER now;
      QueryPerformanceCounter(&now);
      if (!_last_video_time.QuadPart)
        grab_video = true;
      else {
        DWORD interval = DATA_COLLECTION_INTERVAL;
        if (_video_capture_count > SCREEN_CAPTURE_INCREMENTS * 2)
          interval *= 50;
        else if (_video_capture_count > SCREEN_CAPTURE_INCREMENTS)
          interval *= 10;
        LARGE_INTEGER min_time;
        min_time.QuadPart = _last_video_time.QuadPart + 
                              (interval * _ms_frequency.QuadPart);
        if (now.QuadPart >= min_time.QuadPart)
          grab_video = true;
      }
      if (grab_video) {
        _screen_updated = false;
        _last_video_time.QuadPart = now.QuadPart;
        _video_capture_count++;
        _screen_capture.Capture(_document_window, CapturedImage::VIDEO);
      }
    }
  }
}

/*-----------------------------------------------------------------------------
    See if anything has been rendered to the screen
-----------------------------------------------------------------------------*/
void TestState::CheckStartRender() {
  if (!_render_start.QuadPart && _screen_updated && _document_window)
    SetEvent(_check_render_event);
}

/*-----------------------------------------------------------------------------
    Background thread to check to see if rendering has started
    (this way we don't block the browser itself)
-----------------------------------------------------------------------------*/
void TestState::RenderCheckThread() {
  while (!_render_start.QuadPart && !_exit) {
    WaitForSingleObject(_check_render_event, INFINITE);
    if (!_exit) {
      _screen_capture.Lock();
      _screen_updated = false;
      LARGE_INTEGER now;
      QueryPerformanceCounter((LARGE_INTEGER *)&now);

      // grab a screen shot
      bool found = false;
      CapturedImage captured_img(_document_window,CapturedImage::START_RENDER);
      CxImage img;
      if (captured_img.Get(img) && 
          img.GetWidth() > START_RENDER_MARGIN * 2 &&
          img.GetHeight() > START_RENDER_MARGIN * 2) {
        int bpp = img.GetBpp();
        if (bpp >= 15) {
          int height = img.GetHeight();
          int width = img.GetWidth();
          // 24-bit gets a fast-path where we can just compare full rows
          if (bpp <= 24 ) {
            DWORD row_bytes = 3 * (width - (START_RENDER_MARGIN * 2));
            char * white = (char *)malloc(row_bytes);
            if (white) {
              memset(white, 0xFFFFFFFF, row_bytes);
              for (DWORD row = START_RENDER_MARGIN; 
                    row < height - START_RENDER_MARGIN && !found; row++) {
                char * image_bytes = (char *)img.GetBits(row) 
                                      + START_RENDER_MARGIN;
                if (memcmp(image_bytes, white, row_bytes))
                  found = true;
              }
              free (white);
            }
          } else {
            for (DWORD row = START_RENDER_MARGIN; 
                    row < height - START_RENDER_MARGIN && !found; row++) {
              for (DWORD x = START_RENDER_MARGIN; 
                    x < width - START_RENDER_MARGIN && !found; x++) {
                RGBQUAD pixel = img.GetPixelColor(x, row, false);
                if (pixel.rgbBlue != 255 || pixel.rgbRed != 255 || 
                    pixel.rgbGreen != 255)
                  found = true;
              }
            }
          }
        }
      }

      if (found) {
        _render_start.QuadPart = now.QuadPart;
        _screen_capture._captured_images.AddTail(captured_img);
      }
      else
        captured_img.Free();

      _screen_capture.Unlock();
    }
  }
}

/*-----------------------------------------------------------------------------
    Collect the periodic system stats like cpu/memory/bandwidth.
-----------------------------------------------------------------------------*/
void TestState::CollectSystemStats(DWORD ms_from_start, LARGE_INTEGER now) {
  CProgressData data;
  data.ms = ms_from_start;
  DWORD msElapsed = 0;
  if( data.ms > _last_data_ms )
    msElapsed = data.ms - _last_data_ms;
  DWORD elapsed = 0;
  if( now.QuadPart > _last_real_time.QuadPart && _last_real_time.QuadPart)
    elapsed = (DWORD) ((now.QuadPart - _last_real_time.QuadPart) / _ms_frequency.QuadPart);
  _last_real_time = now;
  // figure out the bandwidth
  if( _last_bytes_in )
    data.bpsIn = (_bytes_in - _last_bytes_in) * 800;   // * 100 for the interval and * 8 for Bytes->bits
  _last_bytes_in = _bytes_in;

  // calculate CPU utilization
  FILETIME create, ex, kernel, user;
  if( GetProcessTimes(GetCurrentProcess(), &create, &ex, &kernel, &user) )
  {
    ULARGE_INTEGER k, u;
    k.LowPart = kernel.dwLowDateTime;
    k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;
    u.HighPart = user.dwHighDateTime;
    unsigned __int64 cpuTime = k.QuadPart + u.QuadPart;
    if( _last_process_time && cpuTime >= _last_process_time && elapsed > 0.0)
    {
       // convert it to milli-seconds of CPU delta time
      double delta = (double)(cpuTime - _last_process_time) / (double)10000;
      ATLTRACE2(_T("[wpthook] TestState::CollectSystemStats - cpu-delta elapsed: %f %d \n"), delta, elapsed);
      data.cpu = min((double)delta / (double)elapsed, 1.0) * 100.0;
    }
    _last_process_time = cpuTime;
  }

  // get the memory use (working set - task-manager style)
  PROCESS_MEMORY_COUNTERS mem;
  mem.cb = sizeof(mem);
  if( GetProcessMemoryInfo(GetCurrentProcess(), &mem, sizeof(mem)) )
    data.mem = mem.WorkingSetSize / 1024;

  // interpolate across multiple time periods
  if( msElapsed > 100 )
  {
    DWORD chunks = msElapsed / 100;
    for( DWORD i = 1; i < chunks; i++ )
    {
      CProgressData d;
      d.ms = _last_data_ms + (i * 100);
      d.cpu = data.cpu;               // CPU time was already spread over the period
      d.bpsIn = data.bpsIn / chunks;  // split bandwidth evenly across the time slices
      d.mem = data.mem;               // just assign them all the same memory use (could interpolate but probably not worth it)
      _progress_data.AddTail(d);
    }
    data.bpsIn /= chunks;   // bandwidth is the only measure in the main chunk that needs to be adjusted
  }
  _progress_data.AddTail(data);
}

/*-----------------------------------------------------------------------------
  Collect various performance data and screen capture.
    - See if anything has been rendered to the screen
    - Collect the CPU/memory/BW information
-----------------------------------------------------------------------------*/
void TestState::CollectData() {
  EnterCriticalSection(&_data_cs);
  if (_active) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    DWORD ms = 0;
    if (now.QuadPart > _start.QuadPart)
      ms = (DWORD)((now.QuadPart - _start.QuadPart) / _ms_frequency.QuadPart);
    // round it to the closest interval
    ms = ((DWORD)((ms + (DATA_COLLECTION_INTERVAL / 2)) / 
                  DATA_COLLECTION_INTERVAL)) * DATA_COLLECTION_INTERVAL;
    if (ms != _last_data_ms || !_last_data_ms) {
      _last_data_ms = ms;
      GrabVideoFrame();
      CollectSystemStats(ms, now);
    }
  }
  LeaveCriticalSection(&_data_cs);
}