// @file task.cpp

/**
*    Copyright (C) 2008 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,b
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "pch.h"
#include "task.h"
//#include "../goodies.h"

namespace mongo { 

    namespace task { 

/*
#define MS_VC_EXCEPTION 0x406D1388

#pragma pack(push,8)
typedef struct tagTHREADNAME_INFO
{
   DWORD dwType; // Must be 0x1000.
   LPCSTR szName; // Pointer to name (in user addr space).
   DWORD dwThreadID; // Thread ID (-1=caller thread).
   DWORD dwFlags; // Reserved for future use, must be zero.
} THREADNAME_INFO;
#pragma pack(pop)

void SetThreadName( DWORD dwThreadID, char* threadName)
{
   Sleep(10);
   THREADNAME_INFO info;
   info.dwType = 0x1000;
   info.szName = threadName;
   info.dwThreadID = dwThreadID;
   info.dwFlags = 0;

   __try
   {
      RaiseException( MS_VC_EXCEPTION, 0, sizeof(info)/sizeof(ULONG_PTR), (ULONG_PTR*)&info );
   }
   __except(EXCEPTION_EXECUTE_HANDLER)
   {
   }
}
*/

        Task::Task() { 
            n = 0;
            repeat = 0;
        }

        void Task::halt() { repeat = 0; }

        void Task::run() { 
            //SetThreadName(-1, "A Task");
            assert( n == 0 );
            while( 1 ) {
                n++;
                try { 
                    doWork();
                } 
                catch(...) { }
                if( repeat == 0 )
                    break;
                sleepmillis(repeat);
            }
            me.reset();
        }

        void Task::begin(shared_ptr<Task> t) {
            me = t;
            go();
        }

        void fork(shared_ptr<Task> t) { 
            t->begin(t);
        }

        void repeat(shared_ptr<Task> t, unsigned millis) { 
            t->repeat = millis;
            t->begin(t);
        }
    
    }

}
