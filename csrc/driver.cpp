/* 
   Copyright (c) Microsoft Corporation
   All rights reserved. 

   Licensed under the Apache License, Version 2.0 (the ""License""); you
   may not use this file except in compliance with the License. You may
   obtain a copy of the License at

   http://www.apache.org/licenses/LICENSE-2.0

   THIS CODE IS PROVIDED ON AN *AS IS* BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT
   LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR
   A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.

   See the Apache Version 2.0 License for specific language governing
   permissions and limitations under the License.
*/
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <string.h>

#if defined(_POSIX_VERSION)
#include <sys/resource.h>
#include <sys/times.h>
#endif /* defined(_POSIX_VERSION) */

#ifdef SORA_PLATFORM
#include <winsock2.h> // ws2_32.lib required
#include <ws2tcpip.h>


#include <sora.h>
#include <brick.h>
#include <dspcomm.h>
#include <soratime.h>
#include <windows.h>

#include "sora_radio.h"
#include "sora_threads.h"
#include "sora_thread_queues.h"
#include "sora_ip.h"
#endif

#ifdef BLADE_RF
#include "bladerf_radio.h"
#endif

#ifdef ADI_RF
#include "fmcomms_radio.h"
#endif

#ifdef LIME_RF
#include "lime_radio.h"
#endif

#ifdef USE_FPGA
#include "fpga_modules.h"
#endif

#include "wpl_alloc.h"
#include "buf.h"
#include "utils.h"

#ifdef SORA_PLATFORM
	#define DEBUG_PERF

	TimeMeasurements measurementInfo;

	PSORA_UTHREAD_PROC User_Routines[MAX_THREADS];
    // size_t sizes[MAX_THREADS];

    // set_up_threads is defined in the compiler-generated code
    // and returns the number of threads we set up 
	extern int wpl_set_up_threads(PSORA_UTHREAD_PROC *User_Routines);

#endif

#ifdef __GNUC__
#ifndef __cdecl
	#define __cdecl
#endif
#endif

// Contex blocks
BufContextBlock buf_ctx;
HeapContextBlock heap_ctx;
BufContextBlock *pbuf_ctx = &buf_ctx;
HeapContextBlock *pheap_ctx = &heap_ctx;
int stop_program = 0;

// Blink generated functions 
extern void wpl_input_initialize();
extern void wpl_output_finalize();
extern void wpl_global_init(memsize_int heap_size);
extern int wpl_go();

extern void initBufCtxBlock(BufContextBlock *blk);


// Parameters and parsing
#include "params.h"

/* Global configuration parameters 
***************************************************************************/
BlinkParams Globals;
BlinkParams *params;

// tracks bytes copied 
extern unsigned long long bytes_copied; 

long double get_cpu_time(void)
{
#if defined(CLOCK_PROCESS_CPUTIME_ID)
    struct timespec ts;

    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) == 0)
        return (long double) ts.tv_sec + (long double) ts.tv_nsec / 1e9;
#else
    clock_t cl = clock();

    if (cl != (clock_t) -1)
        return (double) cl / (double) CLOCKS_PER_SEC;
#endif
  return -1;
}

long double get_real_time(void)
{
#if defined(CLOCK_MONOTONIC_RAW)
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) == 0)
        return (long double) ts.tv_sec + (long double) ts.tv_nsec / 1e9;
#else
    clock_t cl = clock();

    if (cl != (clock_t) -1)
        return (double) cl / (double) CLOCKS_PER_SEC;
#endif
    return -1;
}

int __cdecl main(int argc, char **argv) {

  // Initialize the global parameters
  params = &Globals;
  try_parse_args(params, argc, argv);

#ifdef USE_FPGA
  enable_fpga_module();
#endif

  // Start Sora HW
  if (Globals.inType == TY_SDR || Globals.outType == TY_SDR)
  {
#ifdef LIME_RF
	  int ret = LimeRF_RadioStart(params);
	  if (ret < 0) exit(1);
	  if (Globals.inType == TY_SDR)
	  {
		  ret = LimeRF_ConfigureRX(params);
		  if (ret < 0) exit(1);
	  }
	  if (Globals.outType == TY_SDR)
	  {
		  ret = LimeRF_ConfigureTX(params);
		  if (ret < 0) exit(1);
	  }
#endif
#ifdef ADI_RF
	  int ret = Fmcomms_Init(params);
	  if (ret < 0) exit(1);
	  if (Globals.inType == TY_SDR)
	  {
		  ret = Fmcomms_RadioStartRx(params);
		  if (ret < 0) exit(1);
	  }
	  if (Globals.outType == TY_SDR)
	  {
		  ret = Fmcomms_RadioStartTx(params);
		  if (ret < 0) exit(1);
	  }
#endif

#ifdef BLADE_RF

	  if (BladeRF_RadioStart( (Globals.outType == TY_SDR)?params:NULL, (Globals.inType == TY_SDR)?params:NULL ) < 0)
	  {
		  exit(1);
	  }
#endif

#ifdef SORA_RF
	  // SORA
	  RadioStart(&Globals);
	  if (Globals.inType == TY_SDR)
	  {
		  InitSoraRx(params);
	  }
	  if (Globals.outType == TY_SDR)
	  {
		  InitSoraTx(params);
	  }
#endif
  }


#ifdef SORA_PLATFORM
  // Start NDIS
  if (Globals.inType == TY_IP || Globals.outType == TY_IP)
  {
	HRESULT hResult = SoraUEnableGetTxPacket();
	assert(hResult == S_OK);
	Ndis_init(NULL);
  }

  // Start measuring time
  initMeasurementInfo(&(Globals.measurementInfo), Globals.latencyCDFSize);
#endif


  // Init
  initBufCtxBlock(&buf_ctx);
  initHeapCtxBlock(&heap_ctx, Globals.heapSize);

  wpl_global_init(Globals.heapSize);
  wpl_input_initialize();


#ifdef SORA_PLATFORM
  /////////////////////////////////////////////////////////////////////////////  
  // DV: Pass the User_Routines here

  int no_threads = wpl_set_up_threads(User_Routines);

  printf("Setting up threads...\n");

  ULONGLONG ttstart, ttend;

  printf("Starting %d threads...\n", no_threads);
  StartThreads(&ttstart, &ttend, &Globals.measurementInfo.tsinfo, no_threads, User_Routines);

  printf("Total input items (including EOF): %d (%d B), output items: %d (%d B)\n",
	  buf_ctx.total_in, buf_ctx.total_in*buf_ctx.size_in,
	  buf_ctx.total_out, buf_ctx.total_out*buf_ctx.size_out);
  printf("Time Elapsed: %ld us \n",
	  SoraTimeElapsed((ttend / 1000 - ttstart / 1000), &Globals.measurementInfo.tsinfo));

  if (Globals.latencySampling > 0)
  {
	  printf("Min write latency: %ld, max write latency: %ld\n", (ulong)Globals.measurementInfo.minDiff, 
																 (ulong) Globals.measurementInfo.maxDiff);
	  printf("CDF: \n   ");
	  unsigned int i = 0;
	  while (i < Globals.measurementInfo.aDiffPtr)
	  {
		  printf("%ld ", Globals.measurementInfo.aDiff[i]);
		  if (i % 10 == 9)
		  {
			  printf("\n   ");
		  }
		  i++;
	  }
	  printf("\n");
  }


  // Free thread separators
  // NB: these are typically allocated in blink_set_up_threads
  ts_free();

#else
  long double cpu_time_start, cpu_time_end;
  long double real_time_start, real_time_end;

  cpu_time_start = get_cpu_time();
  real_time_start = get_real_time();

  wpl_go();

  cpu_time_end = get_cpu_time();
  real_time_end = get_real_time();

  printf("Time elapsed (usec): %d\n", (int) ((cpu_time_end -
                                              cpu_time_start) *
                                              1000000));
  printf("Elapsed cpu time (sec): %Le\n", cpu_time_end - cpu_time_start);
  printf("Elapsed real time (sec): %Le\n", real_time_end - real_time_start);
#endif

  printf("Bytes copied: %llu \n", bytes_copied);

  wpl_output_finalize();

	// Stop Sora HW
	if (Globals.inType == TY_SDR || Globals.outType == TY_SDR)
	{
#ifdef LIME_RF
		LimeRF_RadioStop(params);
#endif
#ifdef ADI_RF
		Fmcomms_RadioStop(params);
#endif
#ifdef BLADE_RF
		BladeRF_RadioStop(params, params);
#endif
#ifdef SORA_RF
		RadioStop(&Globals);
#endif
	}

#ifdef SORA_PLATFORM
	// Stop NDIS
	if (Globals.inType == TY_IP || Globals.outType == TY_IP)
	{
		if (hUplinkThread != NULL)
		{
			// Sora cleanup.
			SoraUThreadStop(hUplinkThread);
			SoraUThreadFree(hUplinkThread);
		}
		SoraUDisableGetTxPacket();
		// Winsock cleanup.
		closesocket(ConnectSocket);
		WSACleanup();
	}

#endif

#ifdef USE_FPGA
	shutdown_fpga_module();
#endif

  return 0;
}



#ifdef SORA_PLATFORM

BOOLEAN __stdcall go_thread(void * pParam)
{
	thread_info *ti = (thread_info *) pParam;

	wpl_go();

	ti->fRunning = false;

	return false;
}

// Default method. This gets called from test.cpp when there is only one thread.
// Otherwise, test.cpp created its own thread functions 
// and store them in User_Routines array
// These get started from the main 
// Returns the numer of threads 
int SetUpThreads(PSORA_UTHREAD_PROC * User_Routines)
{
	User_Routines[0] = (PSORA_UTHREAD_PROC) go_thread;
	return 1;
}

#else

// Define an empty SetUpThreads() function.
// This is here as a shim for compiling single threaded code with GCC.
// See note in Codegen/CgSetupThreads.hs for more information.
int SetUpThreads(void *unused)
{
  return 1;
}

#endif
