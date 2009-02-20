// processinfo_none.cpp

#include "processinfo.h"

#include <iostream>
using namespace std;

#ifdef _WIN32
int getpid(){
  return 0;
}
#endif

namespace mongo {
    
    ProcessInfo::ProcessInfo( pid_t pid ){
    }

    ProcessInfo::~ProcessInfo(){
    }

    bool ProcessInfo::supported(){
        return false;
    }
    
    int ProcessInfo::getVirtualMemorySize(){
        return -1;
    }
    
    int ProcessInfo::getResidentSize(){
        return -1;
    }

}
