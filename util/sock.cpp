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

    SockAddr::SockAddr(int sourcePort) {
        memset(as<sockaddr_in>().sin_zero, 0, sizeof(as<sockaddr_in>().sin_zero));
        as<sockaddr_in>().sin_family = AF_INET;
        as<sockaddr_in>().sin_port = htons(sourcePort);
        as<sockaddr_in>().sin_addr.s_addr = htonl(INADDR_ANY);
        addressSize = sizeof(sockaddr_in);
    }

    SockAddr::SockAddr(const char * iporhost , int port) {
        if (strchr(iporhost, '/')){
#ifdef _WIN32
            uassert(13080, "no unix socket support on windows", false);
#endif
            uassert(13079, "path to unix socket too long", strlen(iporhost) < sizeof(as<sockaddr_un>().sun_path));
            as<sockaddr_un>().sun_family = AF_UNIX;
            strcpy(as<sockaddr_un>().sun_path, iporhost);
            addressSize = sizeof(sockaddr_un);
        }else if (strchr(iporhost, ':')){
            as<sockaddr_in6>().sin6_family = AF_INET6;
            as<sockaddr_in6>().sin6_port = htons(port);
#ifdef _WIN32
            uassert(13081, "No IPv6 support on windows", false);
#else
            inet_pton(AF_INET6, iporhost, &as<sockaddr_in6>().sin6_addr);
#endif
            addressSize = sizeof(sockaddr_in6);
        } else {
            string ip = hostbyname( iporhost );
            memset(as<sockaddr_in>().sin_zero, 0, sizeof(as<sockaddr_in>().sin_zero));
            as<sockaddr_in>().sin_family = AF_INET;
            as<sockaddr_in>().sin_port = htons(port);
            as<sockaddr_in>().sin_addr.s_addr = inet_addr(ip.c_str());
            addressSize = sizeof(sockaddr_in);
        }
    }
 
    bool SockAddr::isLocalHost() const {
        switch (getType()){
            case AF_INET: return getAddr() == "127.0.0.1";
            case AF_INET6: return getAddr() == "::1";
            case AF_UNIX: return true;
        }
        assert(false);
        return false;
    }

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

    class UDPConnection {
    public:
        UDPConnection() {
            sock = 0;
        }
        ~UDPConnection() {
            if ( sock ) {
                closesocket(sock);
                sock = 0;
            }
        }
        bool init(const SockAddr& myAddr);
        int recvfrom(char *buf, int len, SockAddr& sender);
        int sendto(char *buf, int len, const SockAddr& EndPoint);
        int mtu(const SockAddr& sa) {
            return sa.isLocalHost() ? 16384 : 1480;
        }

        SOCKET sock;
    };

    inline int UDPConnection::recvfrom(char *buf, int len, SockAddr& sender) {
        return ::recvfrom(sock, buf, len, 0, sender.raw(), &sender.addressSize);
    }

    inline int UDPConnection::sendto(char *buf, int len, const SockAddr& EndPoint) {
        if ( 0 && rand() < (RAND_MAX>>4) ) {
            out() << " NOTSENT ";
            return 0;
        }
        return ::sendto(sock, buf, len, 0, EndPoint.raw(), EndPoint.addressSize);
    }

    inline bool UDPConnection::init(const SockAddr& myAddr) {
        sock = socket(myAddr.getType(), SOCK_DGRAM, IPPROTO_UDP);
        if ( sock == INVALID_SOCKET ) {
            out() << "invalid socket? " << OUTPUT_ERRNO << endl;
            return false;
        }
        if ( ::bind(sock, myAddr.raw(), myAddr.addressSize) != 0 ) {
            out() << "udp init failed" << endl;
            closesocket(sock);
            sock = 0;
            return false;
        }
        socklen_t optLen;
        int rcvbuf;
        if (getsockopt(sock,
                       SOL_SOCKET,
                       SO_RCVBUF,
                       (char*)&rcvbuf,
                       &optLen) != -1)
            out() << "SO_RCVBUF:" << rcvbuf << endl;
        return true;
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
