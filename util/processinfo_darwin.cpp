// processinfo_darwin.cpp

#include "processinfo.h"



#include <mach/task_info.h>

#include <mach/mach_init.h>
#include <mach/mach_host.h>
#include <mach/mach_traps.h>
#include <mach/task.h>
#include <mach/vm_map.h>
#include <mach/shared_memory_server.h>
#include <iostream>

using namespace std;

namespace mongo {
    
    ProcessInfo::ProcessInfo( pid_t pid ) : _pid( pid ){
    }

    ProcessInfo::~ProcessInfo(){
    }

    bool ProcessInfo::supported(){
        return true;
    }
    
    int ProcessInfo::getVirtualMemorySize(){
        task_t result;
        
        mach_port_t task;
        
        if ( ( result = task_for_pid( mach_task_self() , _pid , &task) ) != KERN_SUCCESS ){
            cout << "error getting task\n";
            return 0;
        }
        
#if !defined(__LP64__)
        task_basic_info_32 ti;
#else
        task_basic_info_64 ti;
#endif
        mach_msg_type_number_t  count = TASK_BASIC_INFO_COUNT;
        if ( ( result = task_info( task , TASK_BASIC_INFO , (task_info_t)&ti, &count ) )  != KERN_SUCCESS ){
            cout << "error getting task_info: " << result << endl;
            return 0;
        }
        return (int)((double)ti.virtual_size / (1024.0 * 1024 * 2 ) );
    }
    
    int ProcessInfo::getResidentSize(){
        task_t result;
        
        mach_port_t task;
        
        if ( ( result = task_for_pid( mach_task_self() , _pid , &task) ) != KERN_SUCCESS ){
            cout << "error getting task\n";
            return 0;
        }
        
        
#if !defined(__LP64__)
        task_basic_info_32 ti;
#else
        task_basic_info_64 ti;
#endif
        mach_msg_type_number_t  count = TASK_BASIC_INFO_COUNT;
        if ( ( result = task_info( task , TASK_BASIC_INFO , (task_info_t)&ti, &count ) )  != KERN_SUCCESS ){
            cout << "error getting task_info: " << result << endl;
            return 0;
        }
        return (int)( ti.resident_size / (1024 * 1024 ) );
    }

}
