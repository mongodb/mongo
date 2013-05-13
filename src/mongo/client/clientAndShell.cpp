// clientAndShell.cpp

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

#include "mongo/pch.h"

#include "mongo/client/clientOnly-private.h"
#include "mongo/db/client_basic.h"
#include "mongo/db/cmdline.h"
#include "mongo/s/shard.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/timer.h"

namespace mongo {

    CmdLine cmdLine;

    const char * curNs = "in client mode";

    bool dbexitCalled = false;
    // This mutex helps the shell serialize output on exit,
    // to avoid deadlocks at shutdown.  So it also protects
    // the global dbexitCalled.
    namespace shell_utils {
        mongo::mutex &mongoProgramOutputMutex(*(new mongo::mutex("mongoProgramOutputMutex")));
    }

    void dbexit( ExitCode returnCode, const char *whyMsg ) {
        {
            mongo::mutex::scoped_lock lk( shell_utils::mongoProgramOutputMutex );
            dbexitCalled = true;
        }
        out() << "dbexit called" << endl;
        if ( whyMsg )
            out() << " b/c " << whyMsg << endl;
        out() << "exiting" << endl;
        ::_exit( returnCode );
    }

    bool inShutdown() {
        return dbexitCalled;
    }

    string getDbContext() {
        return "in client only mode";
    }

    bool haveLocalShardingInfo( const string& ns ) {
        return false;
    }

    DBClientBase * createDirectClient() {
        uassert( 10256 ,  "no createDirectClient in clientOnly" , 0 );
        return 0;
    }

    void Shard::getAllShards( vector<Shard>& all ) {
        verify(0);
    }

    bool Shard::isAShardNode( const string& ident ) {
        verify(0);
        return false;
    }

    ClientBasic* ClientBasic::getCurrent() {
        return 0;
    }

    bool ClientBasic::hasCurrent() {
        return false;
    }
}
