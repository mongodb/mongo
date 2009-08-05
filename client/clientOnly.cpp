#include "../stdafx.h"

namespace mongo {

    const char * curNs = "in client mode";

    bool quiet = false;

    //    Database* database = 0;

    void dbexit( ExitCode returnCode, const char *whyMsg ) {
        out() << "dbexit called" << endl;
        if ( whyMsg )
            out() << " b/c " << whyMsg << endl;
        out() << "exiting" << endl;
        ::exit( returnCode );
    }


    string getDbContext() {
        return "in client only mode";
    }
}
