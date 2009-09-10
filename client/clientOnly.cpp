#include "../stdafx.h"

namespace mongo {

    const char * curNs = "in client mode";

    bool quiet = false;

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
