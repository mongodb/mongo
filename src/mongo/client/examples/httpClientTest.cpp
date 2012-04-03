// httpClientTest.cpp

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

#include <iostream>

#include "mongo/client/dbclient.h"
#include "util/net/httpclient.h"

using namespace mongo;

void play( string url ) {
    cout << "[" << url << "]" << endl;

    HttpClient c;
    HttpClient::Result r;
    MONGO_verify( c.get( url , &r ) == 200 );

    HttpClient::Headers h = r.getHeaders();
    MONGO_verify( h["Content-Type"].find( "text/html" ) == 0 );

    cout << "\tHeaders" << endl;
    for ( HttpClient::Headers::iterator i = h.begin() ; i != h.end(); ++i ) {
        cout << "\t\t" << i->first << "\t" << i->second << endl;
    }
    
}

int main( int argc, const char **argv ) {

    int port = 27017;
    if ( argc != 1 ) {
        if ( argc != 3 )
            throw -12;
        port = atoi( argv[ 2 ] );
    }
    port += 1000;

    play( str::stream() << "http://localhost:" << port << "/" );
    
#ifdef MONGO_SSL
    play( "https://www.10gen.com/" );
#endif
    
}
