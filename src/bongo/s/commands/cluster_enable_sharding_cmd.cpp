/**
 *    Copyright (C) 2015 BongoDB Inc.
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

#define BONGO_LOG_DEFAULT_COMPONENT ::bongo::logger::LogComponent::kCommand

#include "bongo/platform/basic.h"

#include <set>

#include "bongo/db/audit.h"
#include "bongo/db/auth/action_set.h"
#include "bongo/db/auth/action_type.h"
#include "bongo/db/auth/authorization_manager.h"
#include "bongo/db/auth/authorization_session.h"
#include "bongo/db/client.h"
#include "bongo/db/commands.h"
#include "bongo/s/catalog/sharding_catalog_client.h"
#include "bongo/s/catalog_cache.h"
#include "bongo/s/config.h"
#include "bongo/s/grid.h"
#include "bongo/util/log.h"

namespace bongo {
namespace {

class EnableShardingCmd : public Command {
public:
    EnableShardingCmd() : Command("enableSharding", false, "enablesharding") {}

    virtual bool slaveOk() const {
        return true;
    }

    virtual bool adminOnly() const {
        return true;
    }


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    virtual void help(std::stringstream& help) const {
        help << "Enable sharding for a database. "
             << "(Use 'shardcollection' command afterwards.)\n"
             << "  { enablesharding : \"<dbname>\" }\n";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forDatabaseName(parseNs(dbname, cmdObj)),
                ActionType::enableSharding)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }

        return Status::OK();
    }

    virtual std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const {
        return cmdObj.firstElement().str();
    }

    virtual bool run(OperationContext* txn,
                     const std::string& dbname_unused,
                     BSONObj& cmdObj,
                     int options,
                     std::string& errmsg,
                     BSONObjBuilder& result) {
        const std::string dbname = parseNs("", cmdObj);

        uassert(
            ErrorCodes::InvalidNamespace,
            str::stream() << "invalid db name specified: " << dbname,
            NamespaceString::validDBName(dbname, NamespaceString::DollarInDbNameBehavior::Allow));

        if (dbname == "admin" || dbname == "config" || dbname == "local") {
            errmsg = "can't shard " + dbname + " database";
            return false;
        }

        uassertStatusOK(Grid::get(txn)->catalogClient(txn)->enableSharding(txn, dbname));
        audit::logEnableSharding(Client::getCurrent(), dbname);

        // Make sure to force update of any stale metadata
        Grid::get(txn)->catalogCache()->invalidate(dbname);

        return true;
    }

} enableShardingCmd;

}  // namespace
}  // namespace bongo
