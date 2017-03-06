/**
*    Copyright (C) 2015 10gen Inc.
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
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#include "mongo/platform/basic.h"

#include <sstream>

#include "mongo/db/dbwebserver.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/repl_set_html_summary.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/repl/rslog.h"
#include "mongo/util/mongoutils/html.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace repl {

using namespace html;

class ReplSetHandler : public DbWebHandler {
public:
    ReplSetHandler() : DbWebHandler("_replSet", 1, false) {}

    virtual bool handles(const std::string& url) const {
        return str::startsWith(url, "/_replSet");
    }

    virtual void handle(OperationContext* txn,
                        const char* rq,
                        const std::string& url,
                        BSONObj params,
                        std::string& responseMsg,
                        int& responseCode,
                        std::vector<std::string>& headers,
                        const SockAddr& from) {
        responseMsg = _replSet(txn);
        responseCode = 200;
    }

    /* /_replSet show replica set status in html format */
    std::string _replSet(OperationContext* txn) {
        std::stringstream s;
        s << start("Replica Set Status " + prettyHostName());
        s << p(a("/", "back", "Home") + " | " +
               a("/local/system.replset/?html=1", "", "View Replset Config") + " | " +
               a("/replSetGetStatus?text=1", "", "replSetGetStatus") + " | " +
               a("http://dochub.mongodb.org/core/replicasets", "", "Docs"));

        ReplicationCoordinator* replCoord = getGlobalReplicationCoordinator();
        if (replCoord->getReplicationMode() != ReplicationCoordinator::modeReplSet) {
            s << p("Not using --replSet");
            s << _end();
            return s.str();
        }

        ReplSetHtmlSummary summary;
        replCoord->summarizeAsHtml(&summary);
        s << summary.toHtmlString();

        s << p("Recent replset log activity:");
        fillRsLog(&s);
        s << _end();
        return s.str();
    }

} replSetHandler;

}  // namespace repl
}  // namespace mongo
