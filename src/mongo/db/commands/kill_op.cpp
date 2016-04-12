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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include <limits>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/service_context.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

class KillOpCommand : public Command {
public:
    KillOpCommand() : Command("killOp") {}


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool slaveOk() const final {
        return true;
    }

    bool adminOnly() const final {
        return true;
    }

    Status checkAuthForCommand(ClientBasic* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) final {
        bool isAuthorized = AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
            ResourcePattern::forClusterResource(), ActionType::killop);
        return isAuthorized ? Status::OK() : Status(ErrorCodes::Unauthorized, "Unauthorized");
    }

    bool run(OperationContext* txn,
             const std::string& db,
             BSONObj& cmdObj,
             int options,
             std::string& errmsg,
             BSONObjBuilder& result) final {
        long long op;
        uassertStatusOK(bsonExtractIntegerField(cmdObj, "op", &op));

        log() << "going to kill op: " << op;
        result.append("info", "attempting to kill op");

        // Internally opid is an unsigned 32-bit int, but as BSON only has signed integer types,
        // we wrap values exceeding 2,147,483,647 to negative numbers. The following undoes this
        // transformation, so users can use killOp on the (negative) opid they received.
        if (op >= std::numeric_limits<int>::min() && op < 0)
            op += 1ull << 32;

        uassert(26823,
                str::stream() << "invalid op : " << op,
                (op >= 0) && (op <= std::numeric_limits<unsigned int>::max()));

        getGlobalServiceContext()->killOperation(static_cast<unsigned int>(op));
        return true;
    }
} killOpCmd;

}  // namespace mongo
