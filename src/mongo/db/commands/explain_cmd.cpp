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

#include "mongo/platform/basic.h"

#include "mongo/db/commands.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/rpc/metadata/server_selection_metadata.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::string;

namespace {

/**
 * The explain command is used to generate explain output for any read or write operation which has
 * a query component (e.g. find, count, update, remove, distinct, etc.).
 *
 * The explain command takes as its argument a nested object which specifies the command to
 * explain, and a verbosity indicator. For example:
 *
 *    {explain: {count: "coll", query: {foo: "bar"}}, verbosity: "executionStats"}
 *
 * This command like a dispatcher: it just retrieves a pointer to the nested command and invokes
 * its explain() implementation.
 */
class CmdExplain : public Command {
public:
    CmdExplain() : Command("explain") {}


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    /**
     * Running an explain on a secondary requires explicitly setting slaveOk.
     */
    virtual bool slaveOk() const {
        return false;
    }

    virtual bool slaveOverrideOk() const {
        return true;
    }

    virtual bool maintenanceOk() const {
        return false;
    }

    virtual bool adminOnly() const {
        return false;
    }

    virtual void help(std::stringstream& help) const {
        help << "explain database reads and writes";
    }

    /**
     * You are authorized to run an explain if you are authorized to run
     * the command that you are explaining. The auth check is performed recursively
     * on the nested command.
     */
    virtual Status checkAuthForCommand(ClientBasic* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        if (Object != cmdObj.firstElement().type()) {
            return Status(ErrorCodes::BadValue, "explain command requires a nested object");
        }

        BSONObj explainObj = cmdObj.firstElement().Obj();

        Command* commToExplain = Command::findCommand(explainObj.firstElementFieldName());
        if (NULL == commToExplain) {
            mongoutils::str::stream ss;
            ss << "unknown command: " << explainObj.firstElementFieldName();
            return Status(ErrorCodes::CommandNotFound, ss);
        }

        return commToExplain->checkAuthForCommand(client, dbname, explainObj);
    }

    virtual bool run(OperationContext* txn,
                     const std::string& dbname,
                     BSONObj& cmdObj,
                     int options,
                     std::string& errmsg,
                     BSONObjBuilder& result) {
        ExplainCommon::Verbosity verbosity;
        Status parseStatus = ExplainCommon::parseCmdBSON(cmdObj, &verbosity);
        if (!parseStatus.isOK()) {
            return appendCommandStatus(result, parseStatus);
        }

        // This is the nested command which we are explaining.
        BSONObj explainObj = cmdObj.firstElement().Obj();

        Command* commToExplain = Command::findCommand(explainObj.firstElementFieldName());
        if (NULL == commToExplain) {
            mongoutils::str::stream ss;
            ss << "Explain failed due to unknown command: " << explainObj.firstElementFieldName();
            Status explainStatus(ErrorCodes::CommandNotFound, ss);
            return appendCommandStatus(result, explainStatus);
        }

        // Check whether the child command is allowed to run here. TODO: this logic is
        // copied from Command::execCommand and should be abstracted. Until then, make
        // sure to keep it up to date.
        repl::ReplicationCoordinator* replCoord = repl::getGlobalReplicationCoordinator();
        bool iAmPrimary = replCoord->canAcceptWritesForDatabase(dbname);
        bool commandCanRunOnSecondary = commToExplain->slaveOk();

        bool commandIsOverriddenToRunOnSecondary = commToExplain->slaveOverrideOk() &&
            rpc::ServerSelectionMetadata::get(txn).canRunOnSecondary();
        bool iAmStandalone = !txn->writesAreReplicated();

        const bool canRunHere = iAmPrimary || commandCanRunOnSecondary ||
            commandIsOverriddenToRunOnSecondary || iAmStandalone;

        if (!canRunHere) {
            mongoutils::str::stream ss;
            ss << "Explain's child command cannot run on this node. "
               << "Are you explaining a write command on a secondary?";
            appendCommandStatus(result, false, ss);
            return false;
        }

        // Actually call the nested command's explain(...) method.
        Status explainStatus = commToExplain->explain(
            txn, dbname, explainObj, verbosity, rpc::ServerSelectionMetadata::get(txn), &result);
        if (!explainStatus.isOK()) {
            return appendCommandStatus(result, explainStatus);
        }

        return true;
    }

} cmdExplain;

}  // namespace
}  // namespace mongo
