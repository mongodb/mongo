// sock.cpp

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
#include "sock.h"

namespace mongo {

    static mongo::mutex sock_mutex;

    string hostbyname(const char *hostname) {
        static string unknown = "0.0.0.0";
        if ( unknown == hostname )
            return unknown;

        scoped_lock lk(sock_mutex);
#if defined(_WIN32)
        if( inet_addr(hostname) != INADDR_NONE )
            return hostname;
#else
        struct in_addr temp;
        if ( inet_aton( hostname, &temp ) )
            return hostname;
#endif
        struct hostent *h;
        h = gethostbyname(hostname);
        if ( h == 0 ) return "";
        return inet_ntoa( *((struct in_addr *)(h->h_addr)) );
    }

    void sendtest() {
        out() << "sendtest\n";
        SockAddr me(27016);
        SockAddr dest("127.0.0.1", 27015);
        UDPConnection c;
        if ( c.init(me) ) {
            char buf[256];
            out() << "sendto: ";
            out() << c.sendto(buf, sizeof(buf), dest) << " " << OUTPUT_ERRNO << endl;
        }
        out() << "end\n";
    }

    void listentest() {
        out() << "listentest\n";
        SockAddr me(27015);
        SockAddr sender;
        UDPConnection c;
        if ( c.init(me) ) {
            char buf[256];
            out() << "recvfrom: ";
            out() << c.recvfrom(buf, sizeof(buf), sender) << " " << OUTPUT_ERRNO << endl;
        }
        out() << "end listentest\n";
    }

    void xmain();
    struct SockStartupTests {
        SockStartupTests() {
#if defined(_WIN32)
            WSADATA d;
            if ( WSAStartup(MAKEWORD(2,2), &d) != 0 ) {
                out() << "ERROR: wsastartup failed " << OUTPUT_ERRNO << endl;
                problem() << "ERROR: wsastartup failed " << OUTPUT_ERRNO << endl;
                dbexit( EXIT_NTSERVICE_ERROR );
            }
#endif
        }
    } sstests;

    ListeningSockets* ListeningSockets::_instance = new ListeningSockets();
    
    ListeningSockets* ListeningSockets::get(){
        return _instance;
    }


} // namespace mongo
