/* -*- Mode: C++; c-basic-offset: 4; indent-tabs-mode: t; tab-width: 4 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <windows.h>
#include <stdio.h>
#include <time.h>

#include "vprof.h"

static void cProbe (void* vprofID)
{
    if (_VAL == _IVAR1) _I64VAR1 ++;
    _IVAR1 = _IVAR0;

    if (_VAL == _IVAR0) _I64VAR0 ++;
    _IVAR0 = (int) _VAL;

    _DVAR0 = ((double)_I64VAR0) / _COUNT;
    _DVAR1 = ((double)_I64VAR1) / _COUNT;
}

//__declspec (thread) boolean cv;
//#define if(c) cv = (c); _vprof (cv); if (cv)
//#define if(c) cv = (c); _vprof (cv, cProbe); if (cv)

#define THREADS 1
#define COUNT 100000
#define SLEEPTIME 0

static int64_t evens = 0;
static int64_t odds = 0;

void sub(int val)
{
    int i;
    //_vprof (1);
    for (i = 0; i < COUNT; i++) {
        //_nvprof ("Iteration", 1);
        //_nvprof ("Iteration", 1);
        _vprof (i);
        //_vprof (i);
        //_hprof(i, 3, (int64_t) 1000, (int64_t)2000, (int64_t)3000);
        //_hprof(i, 3, 10000, 10001, 3000000);
        //_nhprof("Event", i, 3, 10000, 10001, 3000000);
        //_nhprof("Event", i, 3, 10000, 10001, 3000000);
        //Sleep(SLEEPTIME);
        if (i % 2 == 0) {
            //_vprof (i);
            ////_hprof(i, 3, 10000, 10001, 3000000);
            //_nvprof ("Iteration", i);
            evens ++;
        } else {
            //_vprof (1);
            _vprof (i, cProbe);
            odds ++;
        }
        //_nvprof ("Iterate", 1);
    }
    //printf("sub %d done.\n", val);
}

HANDLE array[THREADS];

static int run (void)
{
    int i;

    time_t start_time = time(0);

    for (i = 0; i < THREADS; i++) {
        array[i] = CreateThread(0, 0, (LPTHREAD_START_ROUTINE)sub, (LPVOID)i, 0, 0);
    }

    for (i = 0; i < THREADS; i++) {
        WaitForSingleObject(array[i], INFINITE);
    }

    return 0;
}

int main ()
{
    DWORD start, end;

    start = GetTickCount ();
    run ();
    end = GetTickCount ();

    printf ("\nRun took %d msecs\n\n", end-start);
}
