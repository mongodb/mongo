/*    Copyright 2009 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/util/net/hostandport.h"

#include "mongo/bson/util/builder.h"
#include "mongo/db/server_options.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/log.h"
#include "mongo/util/net/sock.h"

namespace mongo {

    HostAndPort::HostAndPort() : _port(-1) {}

    HostAndPort::HostAndPort(const std::string& s) {
        init(s.c_str());
    }
    HostAndPort::HostAndPort(const std::string& h, int p) : _host(h), _port(p) {}

    bool HostAndPort::operator<(const HostAndPort& r) const {
        const int cmp = host().compare(r.host());
        if (cmp)
            return cmp < 0;
        return port() < r.port();
    }

    bool HostAndPort::operator==(const HostAndPort& r) const {
        return host() == r.host() && port() == r.port();
    }

    int HostAndPort::port() const {
        if (hasPort())
            return _port;
        return ServerGlobalParams::DefaultDBPort;
    }

    bool HostAndPort::isLocalHost() const {
        return (  _host == "localhost"
               || str::startsWith(_host.c_str(), "127.")
               || _host == "::1"
               || _host == "anonymous unix socket"
               || _host.c_str()[0] == '/' // unix socket
               );
    }

    HostAndPort HostAndPort::me() {
        const char* ips = serverGlobalParams.bind_ip.c_str();
        while(*ips) {
            std::string ip;
            const char * comma = strchr(ips, ',');
            if (comma) {
                ip = std::string(ips, comma - ips);
                ips = comma + 1;
            }
            else {
                ip = std::string(ips);
                ips = "";
            }
            HostAndPort h = HostAndPort(ip, serverGlobalParams.port);
            if (!h.isLocalHost()) {
                return h;
            }
        }

        std::string h = getHostName();
        verify( !h.empty() );
        verify( h != "localhost" );
        return HostAndPort(h, serverGlobalParams.port);
    }

    std::string HostAndPort::toString() const {
        StringBuilder ss;
        append( ss );
        return ss.str();
    }

    void HostAndPort::append( StringBuilder& ss ) const {
        ss << host() << ':' << port();
    }

    bool HostAndPort::empty() const {
        return _host.empty() && _port < 0;
    }

    void HostAndPort::init(const char *p) {
        uassert(13110, "HostAndPort: host is empty", *p);
        const char *colon = strrchr(p, ':');
        if( colon ) {
            uassert(18522,
                    "HostAndPort: no host component in " + std::string(p),
                    colon != p);
            int port = atoi(colon+1);
            uassert(13095, "HostAndPort: bad port #", port > 0);
            _host = std::string(p,colon-p);
            _port = port;
        }
        else {
            // no port specified.
            _host = p;
            _port = -1;
        }
    }

    std::ostream& operator<<(std::ostream& os, const HostAndPort& hp) {
        return os << hp.toString();
    }

}  // namespace mongo
