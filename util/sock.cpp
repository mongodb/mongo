// @file sock.cpp

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
#include "sock.h"

namespace mongo {

    static mongo::mutex sock_mutex("sock_mutex");

    static bool ipv6 = false;
    void enableIPv6(bool state) { ipv6 = state; }
    bool IPv6Enabled() { return ipv6; }

    string getAddrInfoStrError(int code) { 
#if !defined(_WIN32)
        return gai_strerror(code);
#else
        /* gai_strerrorA is not threadsafe on windows. don't use it. */
        return errnoWithDescription(code);
#endif
    }

    SockAddr::SockAddr(int sourcePort) {
        memset(as<sockaddr_in>().sin_zero, 0, sizeof(as<sockaddr_in>().sin_zero));
        as<sockaddr_in>().sin_family = AF_INET;
        as<sockaddr_in>().sin_port = htons(sourcePort);
        as<sockaddr_in>().sin_addr.s_addr = htonl(INADDR_ANY);
        addressSize = sizeof(sockaddr_in);
    }

    SockAddr::SockAddr(const char * iporhost , int port) {
        if (!strcmp(iporhost, "localhost"))
            iporhost = "127.0.0.1";

        if (strchr(iporhost, '/')){
#ifdef _WIN32
            uassert(13080, "no unix socket support on windows", false);
#endif
            uassert(13079, "path to unix socket too long", strlen(iporhost) < sizeof(as<sockaddr_un>().sun_path));
            as<sockaddr_un>().sun_family = AF_UNIX;
            strcpy(as<sockaddr_un>().sun_path, iporhost);
            addressSize = sizeof(sockaddr_un);
        }else{
            addrinfo* addrs = NULL;
            addrinfo hints;
            memset(&hints, 0, sizeof(addrinfo));
            hints.ai_socktype = SOCK_STREAM;
            //hints.ai_flags = AI_ADDRCONFIG; // This is often recommended but don't do it. SERVER-1579
            hints.ai_flags |= AI_NUMERICHOST; // first pass tries w/o DNS lookup
            hints.ai_family = (IPv6Enabled() ? AF_UNSPEC : AF_INET);

            stringstream ss;
            ss << port;
            int ret = getaddrinfo(iporhost, ss.str().c_str(), &hints, &addrs);

            if (ret == EAI_NONAME ){ 
                // iporhost isn't an IP address, allow DNS lookup
                hints.ai_flags &= ~AI_NUMERICHOST;
                ret = getaddrinfo(iporhost, ss.str().c_str(), &hints, &addrs);
            }

            if (ret){
                log() << "getaddrinfo(\"" << iporhost << "\") failed: " << gai_strerror(ret) << endl;
                *this = SockAddr(port); 
            }else{
                //TODO: handle other addresses in linked list;
                assert(addrs->ai_addrlen <= sizeof(sa));
                memcpy(&sa, addrs->ai_addr, addrs->ai_addrlen);
                addressSize = addrs->ai_addrlen;
                freeaddrinfo(addrs);
            }
        }
    }
 
    bool SockAddr::isLocalHost() const {
        switch (getType()){
            case AF_INET: return getAddr() == "127.0.0.1";
            case AF_INET6: return getAddr() == "::1";
            case AF_UNIX: return true;
            default: return false;
        }
        assert(false);
        return false;
    }

    string hostbyname(const char *hostname) {
        string addr =  SockAddr(hostname, 0).getAddr();
        if (addr == "0.0.0.0")
            return "";
        else
            return addr;
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
            out() << "invalid socket? " << errnoWithDescription() << endl;
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
            out() << c.sendto(buf, sizeof(buf), dest) << " " << errnoWithDescription() << endl;
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
            out() << c.recvfrom(buf, sizeof(buf), sender) << " " << errnoWithDescription() << endl;
        }
        out() << "end listentest\n";
    }

    void xmain();

#if defined(_WIN32)
    namespace {
        struct WinsockInit {
            WinsockInit() {
                WSADATA d;
                if ( WSAStartup(MAKEWORD(2,2), &d) != 0 ) {
                    out() << "ERROR: wsastartup failed " << errnoWithDescription() << endl;
                    problem() << "ERROR: wsastartup failed " << errnoWithDescription() << endl;
                    dbexit( EXIT_NTSERVICE_ERROR );
                }
            }
        } winsock_init;
    }
#endif

    SockAddr unknownAddress( "0.0.0.0", 0 );

    ListeningSockets* ListeningSockets::_instance = new ListeningSockets();
    
    ListeningSockets* ListeningSockets::get(){
        return _instance;
    }

    
    string getHostNameCached(){
        static string host;
        if ( host.empty() ){
            string s = getHostName();
            host = s;
        }
        return host;
    }

} // namespace mongo
