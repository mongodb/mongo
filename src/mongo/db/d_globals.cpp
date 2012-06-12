// @file d_globals.cpp

#include "pch.h"
#include "d_globals.h"
#include "../util/concurrency/rwlock.h"
#include "clientcursor.h"

namespace mongo { 

    DGlobals::DGlobals() :
        clientCursorMonitor( *(new ClientCursorMonitor()) )
    {
    }

    DGlobals d;

}
