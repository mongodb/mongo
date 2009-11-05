// httpclient.cpp

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

#include "stdafx.h"
#include "httpclient.h"

namespace mongo {

    int HttpClient::get( string url , map<string,string>& headers, stringstream& data ){
        uassert( "invalid url" , url.find( "http://" ) == 0 );
        url = url.substr( 7 );
        
        string host , path;
        if ( url.find( "/" ) == string::npos ){
            host = url;
            path = "/";
        }
        else {
            host = url.substr( 0 , url.find( "/" ) );
            path = url.substr( url.find( "/" ) );
        }

        int port = 80;
        uassert( "non standard port not supported yet" , host.find( ":" ) == string::npos );
        
        cout << "host [" << host << "]" << endl;
        cout << "path [" << path << "]" << endl;
        cout << "port: " << port << endl;
        
        string req;
        {
            stringstream ss;
            ss << "GET " << path << " HTTP/1.1\r\n";
            ss << "Host: " << host << "\r\n";
            ss << "Connection: Close\r\n";
            ss << "User-Agent: mongodb http client\r\n";
            ss << "\r\n";

            req = ss.str();
        }

        cout << req << endl;

        return -1;
    }
    
}
