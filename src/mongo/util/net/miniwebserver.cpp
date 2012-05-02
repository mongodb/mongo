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

#include "pch.h"
#include "miniwebserver.h"
#include "../hex.h"

#include "pcrecpp.h"

namespace mongo {

    MiniWebServer::MiniWebServer(const string& name, const string &ip, int port)
        : Listener(name, ip, port, false)
    {}

    string MiniWebServer::parseURL( const char * buf ) {
        const char * urlStart = strchr( buf , ' ' );
        if ( ! urlStart )
            return "/";

        urlStart++;

        const char * end = strchr( urlStart , ' ' );
        if ( ! end ) {
            end = strchr( urlStart , '\r' );
            if ( ! end ) {
                end = strchr( urlStart , '\n' );
            }
        }

        if ( ! end )
            return "/";

        int diff = (int)(end-urlStart);
        if ( diff < 0 || diff > 255 )
            return "/";

        return string( urlStart , (int)(end-urlStart) );
    }

    void MiniWebServer::parseParams( BSONObj & params , string query ) {
        if ( query.size() == 0 )
            return;

        BSONObjBuilder b;
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

            b.append( urlDecode(cur.substr(0,eq)) , urlDecode(cur.substr(eq+1) ) );
        }

        params = b.obj();
    }

    string MiniWebServer::parseMethod( const char * headers ) {
        const char * end = strchr( headers , ' ' );
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

    void MiniWebServer::accepted(boost::shared_ptr<Socket> psock) {
        psock->postFork();
        psock->setTimeout(8);
        char buf[4096];
        int len = 0;
        while ( 1 ) {
            int left = sizeof(buf) - 1 - len;
            if( left == 0 )
                break;
            int x = psock->unsafe_recv( buf + len , left );
            if ( x <= 0 ) {
                psock->close();
                return;
            }
            len += x;
            buf[ len ] = 0;
            if ( fullReceive( buf ) ) {
                break;
            }
        }
        buf[len] = 0;

        string responseMsg;
        int responseCode = 599;
        vector<string> headers;

        try {
            doRequest(buf, parseURL( buf ), responseMsg, responseCode, headers, psock->remoteAddr() );
        }
        catch ( std::exception& e ) {
            responseCode = 500;
            responseMsg = "error loading page: ";
            responseMsg += e.what();
        }
        catch ( ... ) {
            responseCode = 500;
            responseMsg = "unknown error loading page";
        }

        stringstream ss;
        ss << "HTTP/1.0 " << responseCode;
        if ( responseCode == 200 ) ss << " OK";
        ss << "\r\n";
        if ( headers.empty() ) {
            ss << "Content-Type: text/html\r\n";
        }
        else {
            for ( vector<string>::iterator i = headers.begin(); i != headers.end(); i++ ) {
                verify( strncmp("Content-Length", i->c_str(), 14) );
                ss << *i << "\r\n";
            }
        }
        ss << "Connection: close\r\n";
        ss << "Content-Length: " << responseMsg.size() << "\r\n";
        ss << "\r\n";
        ss << responseMsg;
        string response = ss.str();

        try {
            psock->send( response.c_str(), response.size() , "http response" );
            psock->close();
        }
        catch ( SocketException& e ) {
            log(1) << "couldn't send data to http client: " << e << endl;
        }
    }

    string MiniWebServer::getHeader( const char * req , string wanted ) {
        const char * headers = strchr( req , '\n' );
        if ( ! headers )
            return "";
        pcrecpp::StringPiece input( headers + 1 );

        string name;
        string val;
        pcrecpp::RE re("([\\w\\-]+): (.*?)\r?\n");
        while ( re.Consume( &input, &name, &val) ) {
            if ( name == wanted )
                return val;
        }
        return "";
    }

    string MiniWebServer::urlDecode(const char* s) {
        stringstream out;
        while(*s) {
            if (*s == '+') {
                out << ' ';
            }
            else if (*s == '%') {
                out << fromHex(s+1);
                s+=2;
            }
            else {
                out << *s;
            }
            s++;
        }
        return out.str();
    }

} // namespace mongo
