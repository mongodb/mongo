// processinfo.h

#pragma once

#include <sys/types.h>
#include <unistd.h>

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
