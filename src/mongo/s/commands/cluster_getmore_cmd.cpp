/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/getmore_request.h"
#include "mongo/db/stats/counters.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/s/query/cluster_find.h"

namespace mongo {
namespace {

/**
 * Implements the getMore command on mongos. Retrieves more from an existing mongos cursor
 * corresponding to the cursor id passed from the application. In order to generate these results,
 * may issue getMore commands to remote nodes in one or more shards.
 */
class ClusterGetMoreCmd final : public Command {
    MONGO_DISALLOW_COPYING(ClusterGetMoreCmd);

public:
    ClusterGetMoreCmd() : Command("getMore") {}


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool slaveOk() const final {
        return true;
    }

    bool maintenanceOk() const final {
        return false;
    }

    bool adminOnly() const final {
        return false;
    }

    /**
     * A getMore command increments the getMore counter, not the command counter.
     */
    bool shouldAffectCommandCounter() const final {
        return false;
    }

    void help(std::stringstream& help) const final {
        help << "retrieve more documents for a cursor id";
    }

    Status checkAuthForCommand(ClientBasic* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) final {
        StatusWith<GetMoreRequest> parseStatus = GetMoreRequest::parseFromBSON(dbname, cmdObj);
        if (!parseStatus.isOK()) {
            return parseStatus.getStatus();
        }
        const GetMoreRequest& request = parseStatus.getValue();

        return AuthorizationSession::get(client)->checkAuthForGetMore(
            request.nss, request.cursorid, request.term.is_initialized());
    }

    bool run(OperationContext* txn,
             const std::string& dbname,
             BSONObj& cmdObj,
             int options,
             std::string& errmsg,
             BSONObjBuilder& result) final {
        // Counted as a getMore, not as a command.
        globalOpCounters.gotGetMore();

        StatusWith<GetMoreRequest> parseStatus = GetMoreRequest::parseFromBSON(dbname, cmdObj);
        if (!parseStatus.isOK()) {
            return appendCommandStatus(result, parseStatus.getStatus());
        }
        const GetMoreRequest& request = parseStatus.getValue();

        auto response = ClusterFind::runGetMore(txn, request);
        if (!response.isOK()) {
            return appendCommandStatus(result, response.getStatus());
        }

        response.getValue().addToBSON(CursorResponse::ResponseType::SubsequentResponse, &result);
        return true;
    }

} cmdGetMoreCluster;

}  // namespace
}  // namespace mongo
