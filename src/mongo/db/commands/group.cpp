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

#include "mongo/db/commands/group.h"

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/client_basic.h"
#include "mongo/db/exec/group.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/query/get_executor.h"

namespace mongo {

    static GroupCommand cmdGroup;

    GroupCommand::GroupCommand() : Command("group") {}

    Status GroupCommand::checkAuthForCommand(ClientBasic* client,
                                             const std::string& dbname,
                                             const BSONObj& cmdObj) {
        std::string ns = parseNs(dbname, cmdObj);
        if (!client->getAuthorizationSession()->isAuthorizedForActionsOnNamespace(
                NamespaceString(ns), ActionType::find)) {
            return Status(ErrorCodes::Unauthorized, "unauthorized");
        }
        return Status::OK();
    }

    std::string GroupCommand::parseNs(const std::string& dbname, const BSONObj& cmdObj) const {
        const BSONObj& p = cmdObj.firstElement().embeddedObjectUserCheck();
        uassert(17211, "ns has to be set", p["ns"].type() == String);
        return dbname + "." + p["ns"].String();
    }

    Status GroupCommand::parseRequest(const string& dbname,
                                      const BSONObj& cmdObj,
                                      GroupRequest* request) const {
        request->ns = parseNs(dbname, cmdObj);

        const BSONObj& p = cmdObj.firstElement().embeddedObjectUserCheck();

        if (p["cond"].type() == Object) {
            request->query = p["cond"].embeddedObject().getOwned();
        }
        else if (p["condition"].type() == Object) {
            request->query = p["condition"].embeddedObject().getOwned();
        }
        else if (p["query"].type() == Object) {
            request->query = p["query"].embeddedObject().getOwned();
        }
        else if (p["q"].type() == Object) {
            request->query = p["q"].embeddedObject().getOwned();
        }

        if (p["key"].type() == Object) {
            request->keyPattern = p["key"].embeddedObjectUserCheck().getOwned();
            if (!p["$keyf"].eoo()) {
                return Status(ErrorCodes::BadValue, "can't have key and $keyf");
            }
        }
        else if (!p["$keyf"].eoo()) {
            request->keyFunctionCode = p["$keyf"]._asCode();
        }
        else {
            // No key specified.  Use the entire object as the key.
        }

        BSONElement reduce = p["$reduce"];
        if (reduce.eoo()) {
            return Status(ErrorCodes::BadValue, "$reduce has to be set");
        }
        request->reduceCode = reduce._asCode();

        if (reduce.type() == CodeWScope) {
            request->reduceScope = reduce.codeWScopeObject().getOwned();
        }

        if (p["initial"].type() != Object) {
            return Status(ErrorCodes::BadValue, "initial has to be an object");
        }
        request->initial = p["initial"].embeddedObject().getOwned();

        if (!p["finalize"].eoo()) {
            request->finalize = p["finalize"]._asCode();
        }

        return Status::OK();
    }

    bool GroupCommand::run(OperationContext* txn,
                           const std::string& dbname,
                           BSONObj& cmdObj,
                           int,
                           std::string& errmsg,
                           BSONObjBuilder& out,
                           bool fromRepl) {
        GroupRequest groupRequest;
        Status parseRequestStatus = parseRequest(dbname, cmdObj, &groupRequest);
        if (!parseRequestStatus.isOK()) {
            return appendCommandStatus(out, parseRequestStatus);
        }

        Client::ReadContext ctx(txn, groupRequest.ns);
        Database* db = ctx.ctx().db();

        PlanExecutor *rawPlanExecutor;
        Status getExecStatus = getExecutorGroup(txn, db, groupRequest, &rawPlanExecutor); 
        if (!getExecStatus.isOK()) {
            return appendCommandStatus(out, getExecStatus);
        }
        scoped_ptr<PlanExecutor> planExecutor(rawPlanExecutor);

        // Group executors return ADVANCED exactly once, with the entire group result.
        BSONObj retval;
        PlanExecutor::ExecState state = planExecutor->getNext(&retval, NULL);
        if (PlanExecutor::ADVANCED != state) {
            if (PlanExecutor::EXEC_ERROR == state &&
                WorkingSetCommon::isValidStatusMemberObject(retval)) {
                return appendCommandStatus(out, WorkingSetCommon::getMemberObjectStatus(retval));
            }
            return appendCommandStatus(out,
                                       Status(ErrorCodes::BadValue,
                                              str::stream() << "error encountered during group "
                                                            << "operation, executor returned "
                                                            << PlanExecutor::statestr(state)));
        }
        invariant(planExecutor->isEOF());

        invariant(STAGE_GROUP == planExecutor->getRootStage()->stageType());
        GroupStage* groupStage = static_cast<GroupStage*>(planExecutor->getRootStage());
        const GroupStats* groupStats =
            static_cast<const GroupStats*>(groupStage->getSpecificStats());
        const CommonStats* groupChildStats = groupStage->getChildren()[0]->getCommonStats();

        out.appendArray("retval", retval);
        out.append("count", static_cast<long long>(groupChildStats->advanced));
        out.append("keys", static_cast<long long>(groupStats->nGroups));

        return true;
    }

    Status GroupCommand::explain(OperationContext* txn,
                                 const std::string& dbname,
                                 const BSONObj& cmdObj,
                                 Explain::Verbosity verbosity,
                                 BSONObjBuilder* out) const {
        GroupRequest groupRequest;
        Status parseRequestStatus = parseRequest(dbname, cmdObj, &groupRequest);
        if (!parseRequestStatus.isOK()) {
            return parseRequestStatus;
        }

        Client::ReadContext ctx(txn, groupRequest.ns);
        Database* db = ctx.ctx().db();

        PlanExecutor *rawPlanExecutor;
        Status getExecStatus = getExecutorGroup(txn, db, groupRequest, &rawPlanExecutor); 
        if (!getExecStatus.isOK()) {
            return getExecStatus;
        }
        scoped_ptr<PlanExecutor> planExecutor(rawPlanExecutor);

        return Explain::explainStages(planExecutor.get(), verbosity, out);
    }

}  // namespace mongo
