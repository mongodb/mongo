// @file d_globals.h
//
// these are global variables used in mongod ("d").  also used in test binary as that is effectively a variation on mongod code.
// that is, these are not in mongos.
//

#pragma once

namespace mongo { 

    class RWLockRecursive;
    class MongoMutex;
    class ClientCursorMonitor;

    struct DGlobals : boost::noncopyable { 
        DGlobals();

        // these are intentionally never deleted:
        MongoMutex &dbMutex;
        ClientCursorMonitor& clientCursorMonitor;

    };

    extern DGlobals d;

};
