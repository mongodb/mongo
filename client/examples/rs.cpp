// rs.cpp

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

/**
 * example of using replica sets from c++
 */

#include "client/dbclient.h"
#include <iostream>

using namespace mongo;
using namespace std;

int main( int argc , const char ** argv ) {
    string errmsg;
    ConnectionString cs = ConnectionString::parse( "foo/127.0.0.1" , errmsg );
    if ( ! cs.isValid() ) {
        cout << "error parsing url: " << errmsg << endl;
        return 1;
    }

    DBClientReplicaSet * conn = (DBClientReplicaSet*)cs.connect( errmsg );
    if ( ! conn ) {
        cout << "error connecting: " << errmsg << endl;
        return 2;
    }

    string collName = "test.rs1";

    conn->dropCollection( collName );
    while ( true ) {
        try {
            conn->update( collName , BSONObj() , BSON( "$inc" << BSON( "x" << 1 ) ) , true );
            cout << conn->findOne( collName , BSONObj() ) << endl;
            cout << "\t A" << conn->slaveConn().findOne( collName , BSONObj() , 0 , QueryOption_SlaveOk ) << endl;
            cout << "\t B " << conn->findOne( collName , BSONObj() , 0 , QueryOption_SlaveOk ) << endl;
        }
        catch ( std::exception& e ) {
            cout << "ERROR: " << e.what() << endl;
        }
        sleepsecs( 1 );
    }

}
