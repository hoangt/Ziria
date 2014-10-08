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
#include <const.h>
#include <sora.h>
#include "sora_threads.h"
#include "commit_queues.h"

#define ST_BUF_SIZE	64


// Assuming char is 1B
//#define __VALID(buf, size, i) *((bool*) ((buf) + (ST_CACHE_LINE+(size))*(i)))
//#define __DATA(buf, size, i) ((buf) + (ST_CACHE_LINE+(size))*(i) + ST_CACHE_LINE)

volatile bool* cq_valid(char *buf, int size, int i) {
	return ((bool*) ((buf) + (ST_CACHE_LINE+(size))*(i)));
}
char *cq_data(char *buf, int size, int i) {
	return ((buf) + (ST_CACHE_LINE+(size))*(i) + ST_CACHE_LINE);
}





cq_context *cq_contexts;
int cq_no_contexts = 0;


// Sora needed to yield in WinXP because of the scheduler issues
// In Win7 this does not seem necessary. 
// However, after thorough measurements we concluded that 
// SoraThreadYield doesn't affect the performance on many cores
// and help performance on a few cores, so we decided to keep it
//#define USE_SORA_YIELD


//Uncomment to display statistics about cq_queues
//#define CQ_DEBUG

#ifdef CQ_DEBUG
#define MAX_TS	10
__declspec(align(16)) LONG queueSize[MAX_TS*16];
LONG queueCum[MAX_TS];
LONG queueSam[MAX_TS];
LONG almostFull[MAX_TS];
LONG full[MAX_TS];
LONG fstalled[MAX_TS];
LONG esamples[MAX_TS];
LONG empty[MAX_TS];
#endif



// Init <no> queues

cq_context *s_cq_init(int no, size_t *sizes)
{
	cq_context *locCont;

	locCont = (cq_context *) (malloc(no * sizeof(cq_context)));
	if (locCont == NULL)
	{
		return 0;
	}

#ifdef CQ_DEBUG
		memset(queueSize, 0, MAX_TS*16 * sizeof(LONG));
		memset(queueCum, 0, MAX_TS * sizeof(LONG));
		memset(queueSam, 0, MAX_TS * sizeof(LONG));
		memset(almostFull, 0, MAX_TS * sizeof(LONG));
		memset(full, 0, MAX_TS * sizeof(LONG));
		memset(fstalled, 0, MAX_TS * sizeof(LONG));
		memset(esamples, 0, MAX_TS * sizeof(LONG));
		memset(empty, 0, MAX_TS * sizeof(LONG));
#endif

	for (int j=0; j<no; j++)
	{
		// Buffer size should be a multiple of ST_CACHE_LINE
		locCont[j].size = sizes[j];
		locCont[j].alg_size = ST_CACHE_LINE * (sizes[j] / ST_CACHE_LINE);
		if (sizes[j] % ST_CACHE_LINE > 0) locCont[j].alg_size += ST_CACHE_LINE;

		// Allocate one cache line for valid field and the rest for the data
		locCont[j].buf = (char *) _aligned_malloc((ST_CACHE_LINE+locCont[j].alg_size)*ST_BUF_SIZE, ST_CACHE_LINE);
		if (locCont[j].buf == NULL)
		{
			printf("Cannot allocate thread separator buffer! Exiting... \n");
			exit (-1);
		}

		size_t i;
		for (i = 0; i < ST_BUF_SIZE; i++)
		{
			* cq_valid(locCont[j].buf, locCont[j].alg_size, i) = false;
		}
		locCont[j].wptr = locCont[j].rptr = locCont[j].optimistic_rptr = locCont[j].buf;
		locCont[j].evReset = locCont[j].evFlush = locCont[j].evFinish = false;
		locCont[j].evProcessDone = true;
	}

	return locCont;
}



// Init <no> queues
int cq_init(int no, size_t *sizes)
{
	cq_contexts = s_cq_init(no, sizes);
	cq_no_contexts = no;
	return no;
}






// Called by the uplink thread
//        BOOL_FUNC_PROCESS(ipin)
void s_cq_put(cq_context *locCont, int nc, char *input)
{
	/*
	// DEBUG
	printf("%u, nc: %d, wptr: %x, rptr: %x, buf: %x, wptr valid=%d\n", GetCurrentThreadId(), 
		nc, locCont[nc].wptr, locCont[nc].rptr, locCont[nc].buf,
		*valid(locCont[nc].wptr, locCont[nc].alg_size, 0));
	*/

#ifdef CQ_DEBUG
	if (*cq_valid(locCont[nc].wptr, locCont[nc].alg_size, 0)) {
		full[nc]++;
	}
#endif

    // spin wait if the synchronized buffer is full
    //while ((locCont[nc].wptr)->valid) { SoraThreadYield(TRUE); }
    while (*cq_valid(locCont[nc].wptr, locCont[nc].alg_size, 0)) { 
#ifdef CQ_DEBUG
		fstalled[nc]++;
#endif
#ifdef USE_SORA_YIELD
		SoraThreadYield(TRUE); 
#endif
	}


	// copy a burst of input data into the synchronized buffer
	//memcpy ((locCont[nc].wptr)->data, input, sizeof(char)*BURST);
	// Copy only the actual amount of data (size) and not the entire buffer (alg_size)
	memcpy (cq_data(locCont[nc].wptr, locCont[nc].alg_size, 0), input, sizeof(char)*locCont[nc].size);

	*cq_valid(locCont[nc].wptr, locCont[nc].alg_size, 0) = true;
    locCont[nc].evProcessDone = false;


#ifdef CQ_DEBUG
	InterlockedIncrement(queueSize + (nc*16));
	queueCum[nc] += queueSize[nc*16];
	queueSam[nc]++;
	if (queueSize[nc*16] > (ST_BUF_SIZE * 0.9)) almostFull[nc]++;
#endif


	// advance the write pointer
    //(locCont[nc].wptr)++;
    //if ((locCont[nc].wptr) == (locCont[nc].buf) + ST_BUF_SIZE)
    //    (locCont[nc].wptr) = (locCont[nc].buf);
    locCont[nc].wptr += (ST_CACHE_LINE+locCont[nc].alg_size);
    if ((locCont[nc].wptr) == (locCont[nc].buf) + ST_BUF_SIZE*(ST_CACHE_LINE+locCont[nc].alg_size))
        (locCont[nc].wptr) = (locCont[nc].buf);
}





// Called by the uplink thread
//        BOOL_FUNC_PROCESS(ipin)
void cq_put(int nc, char *input)
{
	if (nc >= cq_no_contexts)
	{
		printf("There are only %d queues!, %d\n", cq_no_contexts, nc);
		return;
	}

	s_cq_put(cq_contexts, nc, input);
}





// Called by the downlink thread
bool s_cq_get(cq_context *locCont, int nc, char *output)
{
#ifdef CQ_DEBUG
	esamples[nc]++;
#endif

	// if the synchronized buffer has no data, 
    // check whether there is reset/flush request
    //if (!(locCont[nc].rptr)->valid)
    if (!(*cq_valid(locCont[nc].optimistic_rptr, locCont[nc].alg_size, 0)))
    {		
#ifdef CQ_DEBUG
		empty[nc]++;
#endif
		if (locCont[nc].evReset)
        {
            //Next()->Reset();
            (locCont[nc].evReset) = false;
        }
        if (locCont[nc].evFlush)
        {
            //Next()->Flush();
            locCont[nc].evFlush = false;
        }
        locCont[nc].evProcessDone = true;
		// no data to process  
        return false;
    }
	else
	{
		// Otherwise, there are data. Pump the data to the output pin
		//memcpy ( output, (locCont[nc].rptr)->data, sizeof(char)*BURST);
		// Copy only the actual amount of data (size) and not the entire buffer (alg_size)
		memcpy ( output, cq_data(locCont[nc].optimistic_rptr, locCont[nc].alg_size, 0), sizeof(char)*locCont[nc].size);

        //(locCont[nc].rptr)->valid = false;
	    //(locCont[nc].rptr)++;
        //if ((locCont[nc].rptr) == (locCont[nc].buf) + ST_BUF_SIZE)


#ifdef CQ_DEBUG
		InterlockedDecrement(queueSize + (nc*16));
#endif

		locCont[nc].optimistic_rptr += (ST_CACHE_LINE+locCont[nc].alg_size);
		if ((locCont[nc].optimistic_rptr) == (locCont[nc].buf) + ST_BUF_SIZE*(ST_CACHE_LINE+locCont[nc].alg_size))
        {
            (locCont[nc].optimistic_rptr) = (locCont[nc].buf);
#ifdef USE_SORA_YIELD
            // Periodically yielding in busy processing to prevent OS hanging
            SoraThreadYield(TRUE);
#endif
		}

        //bool rc = Next()->Process(opin0());
        //if (!rc) return rc;
		return true;
	}
    return false;
}


bool cq_get(int nc, char *output)
//FINL bool Process() // ISource::Process
{
	if (nc >= cq_no_contexts)
	{
		printf("There are only %d queues! %d\n", cq_no_contexts, nc);
		return false;
	}

	return s_cq_get(cq_contexts, nc, output);
}







bool s_cq_isFinished(cq_context *locCont, int nc)
{
	// Return true to signal the end
	if (locCont[nc].evFinish)
	{
	  //previously: locCont[nc].evFinish = false;
	  locCont[nc].evProcessDone = true;
	  return true;
	}
	return false;
}

// Called by the downlink thread
bool cq_isFinished(int nc)
{
	if (nc >= cq_no_contexts)
	{
		printf("There are only %d queues!\n", cq_no_contexts);
		return false;
	}

	return s_cq_isFinished(cq_contexts, nc);
}






bool s_cq_isFull(cq_context *locCont, int nc)
{
	return (*cq_valid(locCont[nc].wptr, locCont[nc].alg_size, 0));
}

bool cq_isFull(int nc)
{
	return s_cq_isFull(cq_contexts, nc);
}




bool s_cq_isEmpty(cq_context *locCont, int nc)
{
  return (!(*cq_valid(locCont[nc].optimistic_rptr, locCont[nc].alg_size, 0)));
}

bool cq_isEmpty(int nc)
{
	return s_cq_isEmpty(cq_contexts, nc);
}






// Issued from upstream to downstream
void s_cq_reset(cq_context *locCont, int nc)
{
    // Set the reset event, spin-waiting for 
    // the downstream to process the event
    locCont[nc].evReset = true;
    while (locCont[nc].evReset) { 
#ifdef USE_SORA_YIELD
		SoraThreadYield(TRUE); 
#endif
	}
}

// Issued from upstream to downstream
void cq_reset(int nc)
{
	if (nc >= cq_no_contexts)
	{
		printf("There are only %d queues!\n", cq_no_contexts);
		return;
	}
	s_cq_reset(cq_contexts, nc);
}






// Issued from upstream to downstream
void s_cq_flush(cq_context *locCont, int nc)
{
	// Wait for all data in buf processed by downstreaming bricks
    while (!locCont[nc].evProcessDone) { 
#ifdef USE_SORA_YIELD
		SoraThreadYield(TRUE); 
#endif
	}

    // Set the flush event, spin-waiting for
    // the downstream to process the event
    locCont[nc].evFlush = true;
	while (locCont[nc].evFlush) { 
#ifdef USE_SORA_YIELD
		SoraThreadYield(TRUE); 
#endif
	}
}


// Issued from upstream to downstream
void cq_flush(int nc)
{
	if (nc >= cq_no_contexts)
	{
		printf("There are only %d queues!\n", cq_no_contexts);
		return;
	}

	s_cq_flush(cq_contexts, nc);
}









// Issued from upstream to downstream
void s_cq_finish(cq_context *locCont, int nc)
{
	// Set the reset event, spin-waiting for 
    // the downstream to process the event
    locCont[nc].evFinish = true;
    //previously: while (locCont[nc].evFinish) { SoraThreadYield(TRUE); }
}

// Issued from upstream to downstream
void cq_finish(int nc)
{
	if (nc >= cq_no_contexts)
	{
		printf("There are only %d queues!\n", cq_no_contexts);
		return;
	}
	s_cq_finish(cq_contexts, nc);
}





// We need to set each valid() flag individually so we might as well stuff
// everything into a for loop. Would probably be faster to do all pointer
// updates and the overflow calculation at once, after the loop, instead.
// TODO: optimize this.
void s_cq_commit(cq_context *locCont, int nc, int nelems)
{
	char *buf_end = (locCont[nc].buf) + ST_BUF_SIZE*(ST_CACHE_LINE + locCont[nc].alg_size);
	while (--nelems >= 0)
	{
		*cq_valid(locCont[nc].rptr, locCont[nc].alg_size, 0) = false;
		locCont[nc].rptr += (ST_CACHE_LINE + locCont[nc].alg_size);
		if ((locCont[nc].rptr) == buf_end)
		{
			(locCont[nc].rptr) = (locCont[nc].buf);
#ifdef USE_SORA_YIELD
			// Periodically yielding in busy processing to prevent OS hanging
			SoraThreadYield(TRUE);
#endif
		}
	}
}

void cq_commit(int nc, int nelems) { s_cq_commit(cq_contexts, nc, nelems); }





// Beware the overflow calculation; if there are any bugs here, that's probably where they are.
// TODO: possibly manipulate the valid() stuff here?
void s_cq_rollback(cq_context *locCont, int nc) {
	locCont[nc].optimistic_rptr = locCont[nc].rptr;
}

void cq_rollback(int nc) { s_cq_rollback(cq_contexts, nc); }





void s_cq_free(cq_context *locCont, int no)
{
	for (int nc=0; nc < no; nc++)
	{
#ifdef CQ_DEBUG
	printf("queue=%u, avg queue=%.3f, avg alm.full=%.3f, full=%.3f (%u), fstalled=%u, avg_stall=%.3f, empty=%.3f(%u), total samples=%u\n", 
		(long) queueSize[nc*16], (double) queueCum[nc] / (double) queueSam[nc], 
		(double) almostFull[nc] / (double) queueSam[nc], 
		(double) full[nc] / (double) queueSam[nc], full[nc], fstalled[nc], 
		(double) fstalled[nc] / (double) full[nc], 
		(double) empty[nc] / (double) esamples[nc], empty[nc], 
		queueSam[nc]);
#endif
		_aligned_free(locCont[nc].buf);
	}
}

void cq_free()
{
	s_cq_free(cq_contexts, cq_no_contexts);
}

