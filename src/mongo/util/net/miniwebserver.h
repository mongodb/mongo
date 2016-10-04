// miniwebserver.h

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

#pragma once

#include "mongo/platform/basic.h"


#include "mongo/db/jsobj.h"
#include "mongo/util/net/listen.h"
#include "mongo/util/net/message.h"

namespace mongo {

class ServiceContext;

class MiniWebServer : public Listener {
public:
    MiniWebServer(const std::string& name, const std::string& ip, int _port, ServiceContext* ctx);
    virtual ~MiniWebServer() {}

    virtual void doRequest(const char* rq,  // the full request
                           std::string url,
                           // set these and return them:
                           std::string& responseMsg,
                           int& responseCode,
                           std::vector<std::string>& headers,  // if completely empty, content-type:
                                                               // text/html will be added
                           const SockAddr& from) = 0;

    // --- static helpers ----

    static void parseParams(BSONObj& params, std::string query);

    static std::string parseURL(const char* buf);
    static std::string parseMethod(const char* headers);
    static std::string getHeader(const char* headers, const std::string& name);
    static const char* body(const char* buf);

    static std::string urlDecode(const char* s);
    static std::string urlDecode(const std::string& s) {
        return urlDecode(s.c_str());
    }

    // This is not currently used for the MiniWebServer. See SERVER-24200
    void accepted(std::unique_ptr<AbstractMessagingPort> mp) override;

private:
    void _accepted(const std::shared_ptr<Socket>& psocket, long long connectionId) override;
    static bool fullReceive(const char* buf);
};

}  // namespace mongo
