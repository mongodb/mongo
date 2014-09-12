/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#pragma once

#include "mongo/db/catalog/collection.h"
#include "mongo/db/commands.h"
#include "mongo/db/exec/count.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/repl_coordinator_global.h"

namespace mongo {

    class OperationContext;

    /**
     * 'ns' is the namespace we're counting on.
     *
     * { count: "collectionname"[, query: <query>] }
     *
     * @return -1 on ns does not exist error and other errors, 0 on other errors, otherwise the
     * match count.
     *
     * TODO: This is currently used only by the db direct client. It should be removed.
     */
    long long runCount(OperationContext* txn,
                       const std::string& ns,
                       const BSONObj& cmd,
                       std::string& err,
                       int& errCode);

    /* select count(*) */
    class CmdCount : public Command {
    public:
        virtual bool isWriteCommandForConfigServer() const { return false; }
        CmdCount() : Command("count") { }
        virtual bool slaveOk() const {
            // ok on --slave setups
            return repl::getGlobalReplicationCoordinator()->getSettings().slave == repl::SimpleSlave;
        }
        virtual bool slaveOverrideOk() const { return true; }
        virtual bool maintenanceOk() const { return false; }
        virtual bool adminOnly() const { return false; }
        virtual void help( stringstream& help ) const { help << "count objects in collection"; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::find);
            out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
        }

        virtual Status explain(OperationContext* txn,
                               const std::string& dbname,
                               const BSONObj& cmdObj,
                               Explain::Verbosity verbosity,
                               BSONObjBuilder* out) const;

        virtual bool run(OperationContext* txn,
                         const string& dbname,
                         BSONObj& cmdObj,
                         int, string& errmsg,
                         BSONObjBuilder& result, bool);

        /**
         * Parses a count command object, 'cmdObj'.
         *
         * On success, fills in the out-parameter 'request' and returns an OK status.
         *
         * Returns a failure status if 'cmdObj' is not well formed.
         */
        Status parseRequest(const std::string& dbname,
                            const BSONObj& cmdObj,
                            CountRequest* request) const;

    };

}  // namespace mongo
