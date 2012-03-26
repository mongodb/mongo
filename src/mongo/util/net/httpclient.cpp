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

#include "pch.h"
#include "httpclient.h"
#include "sock.h"
#include "message.h"
#include "message_port.h"
#include "../mongoutils/str.h"
#include "../../bson/util/builder.h"

namespace mongo {

    //#define HD(x) cout << x << endl;
#define HD(x)


    int HttpClient::get( string url , Result * result ) {
        return _go( "GET" , url , 0 , result );
    }

    int HttpClient::post( string url , string data , Result * result ) {
        return _go( "POST" , url , data.c_str() , result );
    }

    int HttpClient::_go( const char * command , string url , const char * body , Result * result ) {
        bool ssl = false;
        if ( url.find( "https://" ) == 0 ) {
            ssl = true;
            url = url.substr( 8 );
        }
        else {
            uassert( 10271 ,  "invalid url" , url.find( "http://" ) == 0 );
            url = url.substr( 7 );
        }

        string host , path;
        if ( url.find( "/" ) == string::npos ) {
            host = url;
            path = "/";
        }
        else {
            host = url.substr( 0 , url.find( "/" ) );
            path = url.substr( url.find( "/" ) );
        }


        HD( "host [" << host << "]" );
        HD( "path [" << path << "]" );

        string server = host;
        int port = ssl ? 443 : 80;

        string::size_type idx = host.find( ":" );
        if ( idx != string::npos ) {
            server = host.substr( 0 , idx );
            string t = host.substr( idx + 1 );
            port = atoi( t.c_str() );
        }

        HD( "server [" << server << "]" );
        HD( "port [" << port << "]" );

        string req;
        {
            stringstream ss;
            ss << command << " " << path << " HTTP/1.1\r\n";
            ss << "Host: " << host << "\r\n";
            ss << "Connection: Close\r\n";
            ss << "User-Agent: mongodb http client\r\n";
            if ( body ) {
                ss << "Content-Length: " << strlen( body ) << "\r\n";
            }
            ss << "\r\n";
            if ( body ) {
                ss << body;
            }

            req = ss.str();
        }

        SockAddr addr( server.c_str() , port );
        HD( "addr: " << addr.toString() );

        Socket sock;
        if ( ! sock.connect( addr ) )
            return -1;
        
        if ( ssl ) {
#ifdef MONGO_SSL
            _checkSSLManager();
            sock.secure( _sslManager.get() );
#else
            uasserted( 15862 , "no ssl support" );
#endif
        }

        {
            const char * out = req.c_str();
            int toSend = req.size();
            sock.send( out , toSend, "_go" );
        }

        char buf[4097];
        int got = sock.unsafe_recv( buf , 4096 );
        buf[got] = 0;

        int rc;
        char version[32];
        verify( sscanf( buf , "%s %d" , version , &rc ) == 2 );
        HD( "rc: " << rc );

        StringBuilder sb;
        if ( result )
            sb << buf;

        while ( ( got = sock.unsafe_recv( buf , 4096 ) ) > 0) {
            buf[got] = 0;
            if ( result )
                sb << buf;
        }

        if ( result ) {
            result->_init( rc , sb.str() );
        }

        return rc;
    }

    void HttpClient::Result::_init( int code , string entire ) {
        _code = code;
        _entireResponse = entire;

        while ( true ) {
            size_t i = entire.find( '\n' );
            if ( i == string::npos ) {
                // invalid
                break;
            }

            string h = entire.substr( 0 , i );
            entire = entire.substr( i + 1 );

            if ( h.size() && h[h.size()-1] == '\r' )
                h = h.substr( 0 , h.size() - 1 );

            if ( h.size() == 0 )
                break;

            i = h.find( ':' );
            if ( i != string::npos )
                _headers[h.substr(0,i)] = str::ltrim(h.substr(i+1));
        }

        _body = entire;
    }

#ifdef MONGO_SSL
    void HttpClient::_checkSSLManager() {
        _sslManager.reset( new SSLManager( true ) );
    }
#endif

}
