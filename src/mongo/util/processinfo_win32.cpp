// processinfo_win32.cpp

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "pch.h"
#include "processinfo.h"
#include <iostream>
#include <psapi.h>
#include "../bson/bsonobjbuilder.h"
using namespace std;

int getpid() {
    return GetCurrentProcessId();
}

namespace mongo {

    int _wconvertmtos( SIZE_T s ) {
        return (int)( s / ( 1024 * 1024 ) );
    }

    ProcessInfo::ProcessInfo( pid_t pid ) {
    }

    ProcessInfo::~ProcessInfo() {
    }

    bool ProcessInfo::supported() {
        return true;
    }

    int ProcessInfo::getVirtualMemorySize() {
        MEMORYSTATUSEX mse;
        mse.dwLength = sizeof(mse);
        verify( 16050, GlobalMemoryStatusEx( &mse ) );
        DWORDLONG x = (mse.ullTotalVirtual - mse.ullAvailVirtual) / (1024 * 1024) ;
        assert( x <= 0x7fffffff );
        return (int) x;
    }

    int ProcessInfo::getResidentSize() {
        PROCESS_MEMORY_COUNTERS pmc;
        verify( 16051, GetProcessMemoryInfo( GetCurrentProcess() , &pmc, sizeof(pmc) ) );
        return _wconvertmtos( pmc.WorkingSetSize );
    }

    void ProcessInfo::getExtraInfo(BSONObjBuilder& info) {
        MEMORYSTATUSEX mse;
        mse.dwLength = sizeof(mse);
        PROCESS_MEMORY_COUNTERS pmc;
        if( GetProcessMemoryInfo( GetCurrentProcess() , &pmc, sizeof(pmc) ) ) {
            info.append("page_faults", static_cast<int>(pmc.PageFaultCount));
            info.append("usagePageFileMB", static_cast<int>(pmc.PagefileUsage / 1024 / 1024));
        }
        if( GlobalMemoryStatusEx( &mse ) ) {
            info.append("totalPageFileMB", static_cast<int>(mse.ullTotalPageFile / 1024 / 1024));
            info.append("availPageFileMB", static_cast<int>(mse.ullAvailPageFile / 1024 / 1024));
            info.append("ramMB", static_cast<int>(mse.ullTotalPhys / 1024 / 1024));
        }
    }

    bool ProcessInfo::blockCheckSupported() {
        return true;
    }

    bool ProcessInfo::blockInMemory( char * start ) {
#if 0
        // code for printing out page fault addresses and pc's --
        // this could be useful for targetting heavy pagefault locations in the code
        static BOOL bstat = InitializeProcessForWsWatch( GetCurrentProcess() );
        PSAPI_WS_WATCH_INFORMATION_EX wiex[30];
        DWORD bufsize =  sizeof(wiex);
        bstat = GetWsChangesEx( GetCurrentProcess(), &wiex[0], &bufsize );
        if (bstat) {
            for (int i=0; i<30; i++) {
                if (wiex[i].BasicInfo.FaultingPc == 0) break;
                cout << "faulting pc = " << wiex[i].BasicInfo.FaultingPc << " address = " << wiex[i].BasicInfo.FaultingVa << " thread id = " << wiex[i].FaultingThreadId << endl;
            }
        }
#endif
        PSAPI_WORKING_SET_EX_INFORMATION wsinfo;
        wsinfo.VirtualAddress = start;
        BOOL result = QueryWorkingSetEx( GetCurrentProcess(), &wsinfo, sizeof(wsinfo) );
        if ( result )
            if ( wsinfo.VirtualAttributes.Valid )
                return true;
        return false;
    }
}
