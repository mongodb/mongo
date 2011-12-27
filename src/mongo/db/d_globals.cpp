// @file d_globals.cpp

#include "pch.h"
#include "d_globals.h"
#include "../util/concurrency/rwlock.h"
#include "clientcursor.h"
#include "mongomutex.h"

namespace mongo { 

    DGlobals::DGlobals() :
        writeExcluder( *(new RWLock("writeexcluder")) ),
        dbMutex( *(new MongoMutex("dbMutex")) ),
        clientCursorMonitor( *(new ClientCursorMonitor()) )
    {
    }

    DGlobals d;

}
