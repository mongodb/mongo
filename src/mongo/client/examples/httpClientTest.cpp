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

#include "mongo/base/init.h"
#include "mongo/client/dbclient.h"
#include "mongo/util/net/httpclient.h"

#ifndef verify
#  define verify(x) MONGO_verify(x)
#endif

using namespace mongo;

void play( string url ) {
    cout << "[" << url << "]" << endl;

    HttpClient c;
    HttpClient::Result r;
    verify( c.get( url , &r ) == 200 );

    HttpClient::Headers h = r.getHeaders();
    verify( h["Content-Type"].find( "text/html" ) == 0 );

    cout << "\tHeaders" << endl;
    for ( HttpClient::Headers::iterator i = h.begin() ; i != h.end(); ++i ) {
        cout << "\t\t" << i->first << "\t" << i->second << endl;
    }
    
}

int main( int argc, const char **argv, char **envp) {

#ifdef MONGO_SSL
    cmdLine.sslOnNormalPorts = true;
    runGlobalInitializersOrDie(argc, argv, envp);
#endif

    int port = 27017;
    if ( argc != 1 ) {
        if ( argc != 3 ) {
            cout << "need to pass port as second param" << endl;
            return EXIT_FAILURE;
        }
        port = atoi( argv[ 2 ] );
    }
    port += 1000;

    play( str::stream() << "http://localhost:" << port << "/" );
    
#ifdef MONGO_SSL
    play( "https://www.mongodb.com/" );
#endif

    return EXIT_SUCCESS;
}
