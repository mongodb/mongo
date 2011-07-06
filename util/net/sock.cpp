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

    static bool ipv6 = false;
    void enableIPv6(bool state) { ipv6 = state; }
    bool IPv6Enabled() { return ipv6; }

    // --- SockAddr

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

        if (strchr(iporhost, '/')) {
#ifdef _WIN32
            uassert(13080, "no unix socket support on windows", false);
#endif
            uassert(13079, "path to unix socket too long", strlen(iporhost) < sizeof(as<sockaddr_un>().sun_path));
            as<sockaddr_un>().sun_family = AF_UNIX;
            strcpy(as<sockaddr_un>().sun_path, iporhost);
            addressSize = sizeof(sockaddr_un);
        }
        else {
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

            // old C compilers on IPv6-capable hosts return EAI_NODATA error
#ifdef EAI_NODATA
            int nodata = (ret == EAI_NODATA);
#else
            int nodata = false;
#endif
            if (ret == EAI_NONAME || nodata) {
                // iporhost isn't an IP address, allow DNS lookup
                hints.ai_flags &= ~AI_NUMERICHOST;
                ret = getaddrinfo(iporhost, ss.str().c_str(), &hints, &addrs);
            }

            if (ret) {
                log() << "getaddrinfo(\"" << iporhost << "\") failed: " << gai_strerror(ret) << endl;
                *this = SockAddr(port);
            }
            else {
                //TODO: handle other addresses in linked list;
                assert(addrs->ai_addrlen <= sizeof(sa));
                memcpy(&sa, addrs->ai_addr, addrs->ai_addrlen);
                addressSize = addrs->ai_addrlen;
                freeaddrinfo(addrs);
            }
        }
    }

    bool SockAddr::isLocalHost() const {
        switch (getType()) {
        case AF_INET: return getAddr() == "127.0.0.1";
        case AF_INET6: return getAddr() == "::1";
        case AF_UNIX: return true;
        default: return false;
        }
        assert(false);
        return false;
    }

    string SockAddr::toString(bool includePort) const {
        string out = getAddr();
        if (includePort && getType() != AF_UNIX && getType() != AF_UNSPEC)
            out += mongoutils::str::stream() << ':' << getPort();
        return out;
    }
    
    sa_family_t SockAddr::getType() const {
        return sa.ss_family;
    }
    
    unsigned SockAddr::getPort() const {
        switch (getType()) {
        case AF_INET:  return ntohs(as<sockaddr_in>().sin_port);
        case AF_INET6: return ntohs(as<sockaddr_in6>().sin6_port);
        case AF_UNIX: return 0;
        case AF_UNSPEC: return 0;
        default: massert(SOCK_FAMILY_UNKNOWN_ERROR, "unsupported address family", false); return 0;
        }
    }
    
    string SockAddr::getAddr() const {
        switch (getType()) {
        case AF_INET:
        case AF_INET6: {
            const int buflen=128;
            char buffer[buflen];
            int ret = getnameinfo(raw(), addressSize, buffer, buflen, NULL, 0, NI_NUMERICHOST);
            massert(13082, errnoWithDescription(ret), ret == 0);
            return buffer;
        }
            
        case AF_UNIX:  return (addressSize > 2 ? as<sockaddr_un>().sun_path : "anonymous unix socket");
        case AF_UNSPEC: return "(NONE)";
        default: massert(SOCK_FAMILY_UNKNOWN_ERROR, "unsupported address family", false); return "";
        }
    }

    bool SockAddr::operator==(const SockAddr& r) const {
        if (getType() != r.getType())
            return false;
        
        if (getPort() != r.getPort())
            return false;
        
        switch (getType()) {
        case AF_INET:  return as<sockaddr_in>().sin_addr.s_addr == r.as<sockaddr_in>().sin_addr.s_addr;
        case AF_INET6: return memcmp(as<sockaddr_in6>().sin6_addr.s6_addr, r.as<sockaddr_in6>().sin6_addr.s6_addr, sizeof(in6_addr)) == 0;
        case AF_UNIX:  return strcmp(as<sockaddr_un>().sun_path, r.as<sockaddr_un>().sun_path) == 0;
        case AF_UNSPEC: return true; // assume all unspecified addresses are the same
        default: massert(SOCK_FAMILY_UNKNOWN_ERROR, "unsupported address family", false);
        }
    }
    
    bool SockAddr::operator!=(const SockAddr& r) const {
        return !(*this == r);
    }
    
    bool SockAddr::operator<(const SockAddr& r) const {
        if (getType() < r.getType())
            return true;
        else if (getType() > r.getType())
            return false;
        
        if (getPort() < r.getPort())
            return true;
        else if (getPort() > r.getPort())
            return false;
        
        switch (getType()) {
        case AF_INET:  return as<sockaddr_in>().sin_addr.s_addr < r.as<sockaddr_in>().sin_addr.s_addr;
        case AF_INET6: return memcmp(as<sockaddr_in6>().sin6_addr.s6_addr, r.as<sockaddr_in6>().sin6_addr.s6_addr, sizeof(in6_addr)) < 0;
        case AF_UNIX:  return strcmp(as<sockaddr_un>().sun_path, r.as<sockaddr_un>().sun_path) < 0;
        case AF_UNSPEC: return false;
        default: massert(SOCK_FAMILY_UNKNOWN_ERROR, "unsupported address family", false);
        }
        
    }

    SockAddr unknownAddress( "0.0.0.0", 0 );


    // ------ hostname -------------------

    string hostbyname(const char *hostname) {
        string addr =  SockAddr(hostname, 0).getAddr();
        if (addr == "0.0.0.0")
            return "";
        else
            return addr;
    }
   
    //  --- my --

    string getHostName() {
        char buf[256];
        int ec = gethostname(buf, 127);
        if ( ec || *buf == 0 ) {
            log() << "can't get this server's hostname " << errnoWithDescription() << endl;
            return "";
        }
        return buf;
    }


    string _hostNameCached;
    static void _hostNameCachedInit() {
        _hostNameCached = getHostName();
    }
    boost::once_flag _hostNameCachedInitFlags = BOOST_ONCE_INIT;

    string getHostNameCached() {
        boost::call_once( _hostNameCachedInit , _hostNameCachedInitFlags );
        return _hostNameCached;
    }

    // --------- SocketException ----------

    string SocketException::toString() const {
        stringstream ss;
        ss << _ei.code << " socket exception [" << _type << "] ";
        
        if ( _server.size() )
            ss << "server [" << _server << "] ";
        
        if ( _extra.size() )
            ss << _extra;
        
        return ss.str();
    }


    // ---------- global init -------------


#if defined(_WIN32)
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
#endif





} // namespace mongo
