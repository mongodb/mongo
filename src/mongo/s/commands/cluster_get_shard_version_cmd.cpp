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

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/s/catalog/catalog_cache.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/config.h"
#include "mongo/s/grid.h"

namespace mongo {

using std::shared_ptr;

namespace {

class GetShardVersion : public Command {
public:
    GetShardVersion() : Command("getShardVersion", false, "getshardversion") {}

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
        help << " example: { getShardVersion : 'alleyinsider.foo'  } ";
    }

    virtual Status checkAuthForCommand(ClientBasic* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forExactNamespace(NamespaceString(parseNs(dbname, cmdObj))),
                ActionType::getShardVersion)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }

        return Status::OK();
    }

    virtual std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const {
        return parseNsFullyQualified(dbname, cmdObj);
    }

    virtual bool run(OperationContext* txn,
                     const std::string& dbname,
                     BSONObj& cmdObj,
                     int options,
                     std::string& errmsg,
                     BSONObjBuilder& result) {
        const NamespaceString nss(parseNs(dbname, cmdObj));
        if (nss.size() == 0) {
            return appendCommandStatus(
                result, Status(ErrorCodes::InvalidNamespace, "no namespace specified"));
        }

        auto status = grid.catalogCache()->getDatabase(txn, nss.db().toString());
        if (!status.isOK()) {
            return appendCommandStatus(result, status.getStatus());
        }

        std::shared_ptr<DBConfig> config = status.getValue();
        if (!config->isSharded(nss.ns())) {
            return appendCommandStatus(
                result,
                Status(ErrorCodes::NamespaceNotSharded, "ns [" + nss.ns() + " is not sharded."));
        }

        ChunkManagerPtr cm = config->getChunkManagerIfExists(txn, nss.ns());
        if (!cm) {
            errmsg = "no chunk manager?";
            return false;
        }

        cm->_printChunks();
        cm->getVersion().addToBSON(result, "version");

        return true;
    }

} getShardVersionCmd;

}  // namespace
}  // namespace mongo
