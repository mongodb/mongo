// processinfo.h

#pragma once

#include <sys/types.h>

#ifndef _WIN32
#include <unistd.h>
#else
typedef int pid_t;
int getpid();
#endif

namespace mongo {
    
    class ProcessInfo {
    public:
        ProcessInfo( pid_t pid = getpid() );
        ~ProcessInfo();
        
        /**
         * @return mbytes
         */
        int getVirtualMemorySize();

        /**
         * @return mbytes
         */
        int getResidentSize();
        
        bool supported();

    private:
        pid_t _pid;
    };

}
