// miniwebserver.cpp

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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/util/net/miniwebserver.h"

#include <pcrecpp.h>

#include "mongo/config.h"
#include "mongo/db/service_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/hex.h"
#include "mongo/util/log.h"
#include "mongo/util/net/socket_exception.h"

namespace mongo {

using std::shared_ptr;
using std::endl;
using std::stringstream;
using std::vector;

MiniWebServer::MiniWebServer(const string& name, const string& ip, int port, ServiceContext* ctx)
    : Listener(name, ip, port, ctx, false, false) {}

string MiniWebServer::parseURL(const char* buf) {
    const char* urlStart = strchr(buf, ' ');
    if (!urlStart)
        return "/";

    urlStart++;

    const char* end = strchr(urlStart, ' ');
    if (!end) {
        end = strchr(urlStart, '\r');
        if (!end) {
            end = strchr(urlStart, '\n');
        }
    }

    if (!end)
        return "/";

    int diff = (int)(end - urlStart);
    if (diff < 0 || diff > 255)
        return "/";

    return string(urlStart, (int)(end - urlStart));
}

void MiniWebServer::parseParams(BSONObj& params, string query) {
    if (query.size() == 0)
        return;

    BSONObjBuilder b;
    while (query.size()) {
        string::size_type amp = query.find("&");

        string cur;
        if (amp == string::npos) {
            cur = query;
            query = "";
        } else {
            cur = query.substr(0, amp);
            query = query.substr(amp + 1);
        }

        string::size_type eq = cur.find("=");
        if (eq == string::npos)
            continue;

        b.append(urlDecode(cur.substr(0, eq)), urlDecode(cur.substr(eq + 1)));
    }

    params = b.obj();
}

string MiniWebServer::parseMethod(const char* headers) {
    const char* end = strchr(headers, ' ');
    if (!end)
        return "GET";
    return string(headers, (int)(end - headers));
}

const char* MiniWebServer::body(const char* buf) {
    const char* ret = strstr(buf, "\r\n\r\n");
    return ret ? ret + 4 : ret;
}

bool MiniWebServer::fullReceive(const char* buf) {
    const char* bod = body(buf);
    if (!bod)
        return false;
    const char* lenString = "Content-Length:";
    const char* lengthLoc = strstr(buf, lenString);
    if (!lengthLoc)
        return true;
    lengthLoc += strlen(lenString);
    long len = strtol(lengthLoc, 0, 10);
    if (long(strlen(bod)) == len)
        return true;
    return false;
}

void MiniWebServer::_accepted(const std::shared_ptr<Socket>& psock, long long connectionId) {
    char buf[4096];
    int len = 0;
    try {
#ifdef MONGO_CONFIG_SSL
        psock->doSSLHandshake();
#endif
        psock->setTimeout(8);
        while (1) {
            int left = sizeof(buf) - 1 - len;
            if (left == 0)
                break;
            int x;
            try {
                x = psock->unsafe_recv(buf + len, left);
            } catch (const SocketException&) {
                psock->close();
                return;
            }
            len += x;
            buf[len] = 0;
            if (fullReceive(buf)) {
                break;
            }
        }
    } catch (const SocketException& e) {
        LOG(1) << "couldn't recv data via http client: " << e << endl;
        return;
    }
    buf[len] = 0;

    string responseMsg;
    int responseCode = 599;
    vector<string> headers;

    try {
        doRequest(buf, parseURL(buf), responseMsg, responseCode, headers, psock->remoteAddr());
    } catch (std::exception& e) {
        responseCode = 500;
        responseMsg = "error loading page: ";
        responseMsg += e.what();
    } catch (...) {
        responseCode = 500;
        responseMsg = "unknown error loading page";
    }

    stringstream ss;
    ss << "HTTP/1.0 " << responseCode;
    if (responseCode == 200)
        ss << " OK";
    ss << "\r\n";
    if (headers.empty()) {
        ss << "Content-Type: text/html\r\n";
    } else {
        for (vector<string>::iterator i = headers.begin(); i != headers.end(); i++) {
            verify(strncmp("Content-Length", i->c_str(), 14));
            ss << *i << "\r\n";
        }
    }
    ss << "Connection: close\r\n";
    ss << "Content-Length: " << responseMsg.size() << "\r\n";
    ss << "\r\n";
    ss << responseMsg;
    string response = ss.str();

    try {
        psock->send(response.c_str(), response.size(), "http response");
        psock->close();
    } catch (SocketException& e) {
        LOG(1) << "couldn't send data to http client: " << e << endl;
    }
}

void MiniWebServer::accepted(std::unique_ptr<AbstractMessagingPort> mp) {
    MONGO_UNREACHABLE;
}

string MiniWebServer::getHeader(const char* req, const std::string& wanted) {
    const char* headers = strchr(req, '\n');
    if (!headers)
        return "";
    pcrecpp::StringPiece input(headers + 1);

    string name;
    string val;
    pcrecpp::RE re("([\\w\\-]+): (.*?)\r?\n");
    while (re.Consume(&input, &name, &val)) {
        if (name == wanted)
            return val;
    }
    return "";
}

string MiniWebServer::urlDecode(const char* s) {
    stringstream out;
    while (*s) {
        if (*s == '+') {
            out << ' ';
        } else if (*s == '%') {
            out << fromHex(s + 1);
            s += 2;
        } else {
            out << *s;
        }
        s++;
    }
    return out.str();
}

}  // namespace mongo
