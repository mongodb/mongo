// miniwebserver.cpp

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
#include "miniwebserver.h"

#include <pcrecpp.h>

namespace mongo {

    MiniWebServer::MiniWebServer() {
        sock = 0;
    }

    bool MiniWebServer::init(const string &ip, int _port) {
        port = _port;
        SockAddr me;
        if ( ip.empty() )
            me = SockAddr( port );
        else
            me = SockAddr( ip.c_str(), port );
        sock = ::socket(AF_INET, SOCK_STREAM, 0);
        if ( sock == INVALID_SOCKET ) {
            log() << "ERROR: MiniWebServer listen(): invalid socket? " << errno << endl;
            return false;
        }
        prebindOptions( sock );
        if ( ::bind(sock, (sockaddr *) &me.sa, me.addressSize) != 0 ) {
            log() << "MiniWebServer: bind() failed port:" << port << " errno:" << errno << endl;
            if ( errno == 98 )
                log() << "98 == addr already in use" << endl;
            closesocket(sock);
            return false;
        }

        if ( ::listen(sock, 16) != 0 ) {
            log() << "MiniWebServer: listen() failed " << errno << endl;
            closesocket(sock);
            return false;
        }

        return true;
    }

    string MiniWebServer::parseURL( const char * buf ) {
        const char * urlStart = strstr( buf , " " );
        if ( ! urlStart )
            return "/";

        urlStart++;

        const char * end = strstr( urlStart , " " );
        if ( ! end ) {
            end = strstr( urlStart , "\r" );
            if ( ! end ) {
                end = strstr( urlStart , "\n" );
            }
        }

        if ( ! end )
            return "/";

        int diff = (int)(end-urlStart);
        if ( diff < 0 || diff > 255 )
            return "/";

        return string( urlStart , (int)(end-urlStart) );
    }

    void MiniWebServer::parseParams( map<string,string> & params , string query ) {
        if ( query.size() == 0 )
            return;

        while ( query.size() ) {

            string::size_type amp = query.find( "&" );

            string cur;
            if ( amp == string::npos ) {
                cur = query;
                query = "";
            }
            else {
                cur = query.substr( 0 , amp );
                query = query.substr( amp + 1 );
            }

            string::size_type eq = cur.find( "=" );
            if ( eq == string::npos )
                continue;

            params[cur.substr(0,eq)] = cur.substr(eq+1);
        }
        return;
    }

    string MiniWebServer::parseMethod( const char * headers ) {
        const char * end = strstr( headers , " " );
        if ( ! end )
            return "GET";
        return string( headers , (int)(end-headers) );
    }

    const char *MiniWebServer::body( const char *buf ) {
        const char *ret = strstr( buf, "\r\n\r\n" );
        return ret ? ret + 4 : ret;
    }

    bool MiniWebServer::fullReceive( const char *buf ) {
        const char *bod = body( buf );
        if ( !bod )
            return false;
        const char *lenString = "Content-Length:";
        const char *lengthLoc = strstr( buf, lenString );
        if ( !lengthLoc )
            return true;
        lengthLoc += strlen( lenString );
        long len = strtol( lengthLoc, 0, 10 );
        if ( long( strlen( bod ) ) == len )
            return true;
        return false;
    }

    void MiniWebServer::accepted(int s, const SockAddr &from) {
        char buf[4096];
        int len = 0;
        while ( 1 ) {
            int x = ::recv(s, buf + len, sizeof(buf) - 1 - len, 0);
            if ( x <= 0 ) {
                return;
            }
            len += x;
            buf[ len ] = 0;
            if ( fullReceive( buf ) )
                break;
        }
        buf[len] = 0;

        string responseMsg;
        int responseCode = 599;
        vector<string> headers;
        doRequest(buf, parseURL( buf ), responseMsg, responseCode, headers, from);

        stringstream ss;
        ss << "HTTP/1.0 " << responseCode;
        if ( responseCode == 200 ) ss << " OK";
        ss << "\r\n";
        if ( headers.empty() ) {
            ss << "Content-Type: text/html\r\n";
        }
        else {
            for ( vector<string>::iterator i = headers.begin(); i != headers.end(); i++ )
                ss << *i << "\r\n";
        }
        ss << "\r\n";
        ss << responseMsg;
        string response = ss.str();

        ::send(s, response.c_str(), response.size(), 0);
    }
    
    string MiniWebServer::getHeader( const char * req , string wanted ){
        const char * headers = strstr( req , "\n" );
        if ( ! headers )
            return "";
        pcrecpp::StringPiece input( headers + 1 );
        
        string name;
        string val;
        pcrecpp::RE re("([\\w\\-]+): (.*?)\r?\n");
        while ( re.Consume( &input, &name, &val) ){
            if ( name == wanted )
                return val;
        }
        return "";
    }
    
    void MiniWebServer::run() {
        SockAddr from;
        while ( 1 ) {
            int s = accept(sock, (sockaddr *) &from.sa, &from.addressSize);
            if ( s < 0 ) {
                if ( errno == ECONNABORTED ) {
                    log() << "Listener on port " << port << " aborted." << endl;
                    return;
                }
                log() << "MiniWebServer: accept() returns " << s << " errno:" << errno << endl;
                sleepmillis(200);
                continue;
            }
            disableNagle(s);
            RARELY log() << "MiniWebServer: connection accepted from " << from.toString() << endl;
            accepted( s, from );
            closesocket(s);
        }
    }

} // namespace mongo
