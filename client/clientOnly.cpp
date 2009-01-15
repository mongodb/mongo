
#include <iostream>

using namespace std;

namespace mongo {

    const char * curNs = "in client mode";
    //    Database* database = 0;

    void dbexit(int returnCode, const char *whyMsg ) {
        cout << "dbexit called" << endl;
        if ( whyMsg )
            cout << " b/c " << whyMsg << endl;
        cout << "exiting" << endl;
        exit( returnCode );
    }


    string getDbContext() {
        return "in client only mode";
    }
}
