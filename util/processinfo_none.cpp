// processinfo_none.cpp

#include "processinfo.h"

#include <iostream>
using namespace std;

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
