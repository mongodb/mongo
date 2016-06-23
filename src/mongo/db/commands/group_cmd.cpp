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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/group.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/util/log.h"

namespace mongo {

using std::unique_ptr;
using std::string;

namespace {

/**
 * The group command is deprecated. Users should prefer the aggregation framework or mapReduce. See
 * http://dochub.mongodb.org/core/group-command-deprecation for more detail.
 */
class GroupCommand : public Command {
public:
    GroupCommand() : Command("group") {}

private:
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    virtual bool maintenanceOk() const {
        return false;
    }

    virtual bool slaveOk() const {
        return false;
    }

    virtual bool slaveOverrideOk() const {
        return true;
    }

    bool supportsReadConcern() const final {
        return true;
    }

    ReadWriteType getReadWriteType() const {
        return ReadWriteType::kRead;
    }

    std::size_t reserveBytesForReply() const override {
        return FindCommon::kInitReplyBufferSize;
    }

    virtual void help(std::stringstream& help) const {
        help << "http://dochub.mongodb.org/core/aggregation";
    }

    virtual Status checkAuthForCommand(ClientBasic* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        std::string ns = parseNs(dbname, cmdObj);
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnNamespace(
                NamespaceString(ns), ActionType::find)) {
            return Status(ErrorCodes::Unauthorized, "unauthorized");
        }
        return Status::OK();
    }

    virtual std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const {
        const BSONObj& p = cmdObj.firstElement().embeddedObjectUserCheck();
        uassert(17211, "ns has to be set", p["ns"].type() == String);
        return dbname + "." + p["ns"].String();
    }

    virtual Status explain(OperationContext* txn,
                           const std::string& dbname,
                           const BSONObj& cmdObj,
                           ExplainCommon::Verbosity verbosity,
                           const rpc::ServerSelectionMetadata&,
                           BSONObjBuilder* out) const {
        GroupRequest groupRequest;
        Status parseRequestStatus = _parseRequest(dbname, cmdObj, &groupRequest);
        if (!parseRequestStatus.isOK()) {
            return parseRequestStatus;
        }

        groupRequest.explain = true;

        AutoGetCollectionForRead ctx(txn, groupRequest.ns);
        Collection* coll = ctx.getCollection();

        auto statusWithPlanExecutor =
            getExecutorGroup(txn, coll, groupRequest, PlanExecutor::YIELD_AUTO);
        if (!statusWithPlanExecutor.isOK()) {
            return statusWithPlanExecutor.getStatus();
        }

        unique_ptr<PlanExecutor> planExecutor = std::move(statusWithPlanExecutor.getValue());

        Explain::explainStages(planExecutor.get(), coll, verbosity, out);
        return Status::OK();
    }

    virtual bool run(OperationContext* txn,
                     const std::string& dbname,
                     BSONObj& cmdObj,
                     int options,
                     std::string& errmsg,
                     BSONObjBuilder& result) {
        RARELY {
            warning() << "The group command is deprecated. See "
                         "http://dochub.mongodb.org/core/group-command-deprecation.";
        }

        GroupRequest groupRequest;
        Status parseRequestStatus = _parseRequest(dbname, cmdObj, &groupRequest);
        if (!parseRequestStatus.isOK()) {
            return appendCommandStatus(result, parseRequestStatus);
        }

        AutoGetCollectionForRead ctx(txn, groupRequest.ns);
        Collection* coll = ctx.getCollection();

        auto statusWithPlanExecutor =
            getExecutorGroup(txn, coll, groupRequest, PlanExecutor::YIELD_AUTO);
        if (!statusWithPlanExecutor.isOK()) {
            return appendCommandStatus(result, statusWithPlanExecutor.getStatus());
        }

        unique_ptr<PlanExecutor> planExecutor = std::move(statusWithPlanExecutor.getValue());

        auto curOp = CurOp::get(txn);
        {
            stdx::lock_guard<Client>(*txn->getClient());
            curOp->setPlanSummary_inlock(Explain::getPlanSummary(planExecutor.get()));
        }

        // Group executors return ADVANCED exactly once, with the entire group result.
        BSONObj retval;
        PlanExecutor::ExecState state = planExecutor->getNext(&retval, NULL);
        if (PlanExecutor::ADVANCED != state) {
            invariant(PlanExecutor::FAILURE == state || PlanExecutor::DEAD == state);

            if (WorkingSetCommon::isValidStatusMemberObject(retval)) {
                return appendCommandStatus(result, WorkingSetCommon::getMemberObjectStatus(retval));
            }
            return appendCommandStatus(result,
                                       Status(ErrorCodes::BadValue,
                                              str::stream() << "error encountered during group "
                                                            << "operation, executor returned "
                                                            << PlanExecutor::statestr(state)));
        }

        invariant(planExecutor->isEOF());

        PlanSummaryStats summaryStats;
        Explain::getSummaryStats(*planExecutor, &summaryStats);
        if (coll) {
            coll->infoCache()->notifyOfQuery(txn, summaryStats.indexesUsed);
        }
        curOp->debug().setPlanSummaryMetrics(summaryStats);

        if (curOp->shouldDBProfile(curOp->elapsedMillis())) {
            BSONObjBuilder execStatsBob;
            Explain::getWinningPlanStats(planExecutor.get(), &execStatsBob);
            curOp->debug().execStats = execStatsBob.obj();
        }

        invariant(STAGE_GROUP == planExecutor->getRootStage()->stageType());
        GroupStage* groupStage = static_cast<GroupStage*>(planExecutor->getRootStage());
        const GroupStats* groupStats =
            static_cast<const GroupStats*>(groupStage->getSpecificStats());
        const CommonStats* groupChildStats = groupStage->getChildren()[0]->getCommonStats();

        result.appendArray("retval", retval);
        result.append("count", static_cast<long long>(groupChildStats->advanced));
        result.append("keys", static_cast<long long>(groupStats->nGroups));

        return true;
    }

private:
    /**
     * Parse a group command object.
     *
     * If 'cmdObj' is well-formed, returns Status::OK() and fills in out-argument 'request'.
     *
     * If a parsing error is encountered, returns an error Status.
     */
    Status _parseRequest(const std::string& dbname,
                         const BSONObj& cmdObj,
                         GroupRequest* request) const {
        request->ns = parseNs(dbname, cmdObj);

        // By default, group requests are regular group not explain of group.
        request->explain = false;

        const BSONObj& p = cmdObj.firstElement().embeddedObjectUserCheck();

        if (p["cond"].type() == Object) {
            request->query = p["cond"].embeddedObject().getOwned();
        } else if (p["condition"].type() == Object) {
            request->query = p["condition"].embeddedObject().getOwned();
        } else if (p["query"].type() == Object) {
            request->query = p["query"].embeddedObject().getOwned();
        } else if (p["q"].type() == Object) {
            request->query = p["q"].embeddedObject().getOwned();
        }

        if (p["key"].type() == Object) {
            request->keyPattern = p["key"].embeddedObjectUserCheck().getOwned();
            if (!p["$keyf"].eoo()) {
                return Status(ErrorCodes::BadValue, "can't have key and $keyf");
            }
        } else if (!p["$keyf"].eoo()) {
            request->keyFunctionCode = p["$keyf"]._asCode();
        } else {
            // No key specified.  Use the entire object as the key.
        }

        BSONElement collationElt;
        Status collationEltStatus =
            bsonExtractTypedField(p, "collation", BSONType::Object, &collationElt);
        if (!collationEltStatus.isOK() && (collationEltStatus != ErrorCodes::NoSuchKey)) {
            return collationEltStatus;
        }
        if (collationEltStatus.isOK()) {
            request->collation = collationElt.embeddedObject().getOwned();
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

} cmdGroup;

}  // namespace
}  // namespace mongo
