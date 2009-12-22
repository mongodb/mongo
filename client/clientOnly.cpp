// clientOnly.cpp

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

#include "../stdafx.h"
#include "../db/cmdline.h"

namespace mongo {

    const char * curNs = "in client mode";

    bool quiet = false;

    CmdLine cmdLine;

    //    Database* database = 0;
    
    bool dbexitCalled = false;

    void dbexit( ExitCode returnCode, const char *whyMsg ) {
        dbexitCalled = true;
        out() << "dbexit called" << endl;
        if ( whyMsg )
            out() << " b/c " << whyMsg << endl;
        out() << "exiting" << endl;
        ::exit( returnCode );
    }
    
    bool inShutdown(){
        return dbexitCalled;
    }

    string getDbContext() {
        return "in client only mode";
    }

    bool haveLocalShardingInfo( const string& ns ){
        return false;
    }
}
