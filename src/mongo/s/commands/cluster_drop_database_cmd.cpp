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

#include "mongo/base/status.h"
#include "mongo/db/commands.h"
#include "mongo/s/config.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

    class DropDatabaseCmd : public Command {
    public:
        DropDatabaseCmd() : Command("dropDatabase") { }

        virtual bool slaveOk() const {
            return true;
        }

        virtual bool adminOnly() const {
            return false;
        }

        virtual bool isWriteCommandForConfigServer() const {
            return false;
        }

        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {

            ActionSet actions;
            actions.addAction(ActionType::dropDatabase);
            out->push_back(Privilege(ResourcePattern::forDatabaseName(dbname), actions));
        }

        virtual bool run(OperationContext* txn,
                         const std::string& dbname,
                         BSONObj& cmdObj,
                         int options,
                         std::string& errmsg,
                         BSONObjBuilder& result,
                         bool fromRepl) {

            // Disallow dropping the config database from mongos
            if (dbname == "config") {
                return appendCommandStatus(result, Status(ErrorCodes::IllegalOperation,
                                                          "Cannot drop the config database"));
            }

            BSONElement e = cmdObj.firstElement();

            if (!e.isNumber() || e.number() != 1) {
                errmsg = "invalid params";
                return 0;
            }

            DBConfigPtr conf = grid.getDBConfig(dbname, false);

            log() << "DROP DATABASE: " << dbname;

            if (!conf) {
                result.append("info", "database didn't exist");
                return true;
            }

            //
            // Reload the database configuration so that we're sure a database entry exists
            // TODO: This won't work with parallel dropping
            //

            grid.removeDBIfExists(*conf);
            grid.getDBConfig(dbname);

            // TODO: Make dropping logic saner and more tolerant of partial drops.  This is
            // particularly important since a database drop can be aborted by *any* collection
            // with a distributed namespace lock taken (migrates/splits)

            //
            // Create a copy of the DB config object to drop, so that no one sees a weird
            // intermediate version of the info
            //

            DBConfig confCopy(conf->name());
            if (!confCopy.load()) {
                errmsg = "could not load database info to drop";
                return false;
            }

            // Enable sharding so we can correctly retry failed drops
            // This will re-drop old sharded entries if they exist
            confCopy.enableSharding(false);

            if (!confCopy.dropDatabase(errmsg)) {
                return false;
            }

            result.append("dropped", dbname);
            return true;
        }

    } clusterDropDatabaseCmd;

} // namespace
} // namespace mongo
