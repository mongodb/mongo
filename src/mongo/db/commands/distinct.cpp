// distinct.cpp

/**
*    Copyright (C) 2012-2014 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include <string>
#include <vector>

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/instance.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/parsed_distinct.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"

namespace mongo {

using std::unique_ptr;
using std::string;
using std::stringstream;

namespace dps = ::mongo::dotted_path_support;

class DistinctCommand : public Command {
public:
    DistinctCommand() : Command("distinct") {}

    virtual bool slaveOk() const {
        return false;
    }

    virtual bool slaveOverrideOk() const {
        return true;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
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

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::find);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }

    virtual void help(stringstream& help) const {
        help << "{ distinct : 'collection name' , key : 'a.b' , query : {} }";
    }

    virtual Status explain(OperationContext* txn,
                           const std::string& dbname,
                           const BSONObj& cmdObj,
                           ExplainCommon::Verbosity verbosity,
                           const rpc::ServerSelectionMetadata&,
                           BSONObjBuilder* out) const {
        const string ns = parseNs(dbname, cmdObj);
        const NamespaceString nss(ns);

        const ExtensionsCallbackReal extensionsCallback(txn, &nss);
        auto parsedDistinct = ParsedDistinct::parse(txn, nss, cmdObj, extensionsCallback, true);
        if (!parsedDistinct.isOK()) {
            return parsedDistinct.getStatus();
        }

        AutoGetCollectionForRead ctx(txn, ns);

        Collection* collection = ctx.getCollection();

        auto executor = getExecutorDistinct(
            txn, collection, ns, &parsedDistinct.getValue(), PlanExecutor::YIELD_AUTO);
        if (!executor.isOK()) {
            return executor.getStatus();
        }

        Explain::explainStages(executor.getValue().get(), collection, verbosity, out);
        return Status::OK();
    }

    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& cmdObj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        const string ns = parseNs(dbname, cmdObj);
        const NamespaceString nss(ns);

        const ExtensionsCallbackReal extensionsCallback(txn, &nss);
        auto parsedDistinct = ParsedDistinct::parse(txn, nss, cmdObj, extensionsCallback, false);
        if (!parsedDistinct.isOK()) {
            return appendCommandStatus(result, parsedDistinct.getStatus());
        }

        AutoGetCollectionForRead ctx(txn, ns);

        Collection* collection = ctx.getCollection();

        auto executor = getExecutorDistinct(
            txn, collection, ns, &parsedDistinct.getValue(), PlanExecutor::YIELD_AUTO);
        if (!executor.isOK()) {
            return appendCommandStatus(result, executor.getStatus());
        }

        {
            stdx::lock_guard<Client>(*txn->getClient());
            CurOp::get(txn)->setPlanSummary_inlock(
                Explain::getPlanSummary(executor.getValue().get()));
        }

        string key = cmdObj[ParsedDistinct::kKeyField].valuestrsafe();

        int bufSize = BSONObjMaxUserSize - 4096;
        BufBuilder bb(bufSize);
        char* start = bb.buf();

        BSONArrayBuilder arr(bb);
        BSONElementSet values(executor.getValue()->getCanonicalQuery()->getCollator());

        BSONObj obj;
        PlanExecutor::ExecState state;
        while (PlanExecutor::ADVANCED == (state = executor.getValue()->getNext(&obj, NULL))) {
            // Distinct expands arrays.
            //
            // If our query is covered, each value of the key should be in the index key and
            // available to us without this.  If a collection scan is providing the data, we may
            // have to expand an array.
            BSONElementSet elts;
            dps::extractAllElementsAlongPath(obj, key, elts);

            for (BSONElementSet::iterator it = elts.begin(); it != elts.end(); ++it) {
                BSONElement elt = *it;
                if (values.count(elt)) {
                    continue;
                }
                int currentBufPos = bb.len();

                uassert(17217,
                        "distinct too big, 16mb cap",
                        (currentBufPos + elt.size() + 1024) < bufSize);

                arr.append(elt);
                BSONElement x(start + currentBufPos);
                values.insert(x);
            }
        }

        // Return an error if execution fails for any reason.
        if (PlanExecutor::FAILURE == state || PlanExecutor::DEAD == state) {
            log() << "Plan executor error during distinct command: "
                  << PlanExecutor::statestr(state)
                  << ", stats: " << Explain::getWinningPlanStats(executor.getValue().get());

            return appendCommandStatus(result,
                                       Status(ErrorCodes::OperationFailed,
                                              str::stream()
                                                  << "Executor error during distinct command: "
                                                  << WorkingSetCommon::toStatusString(obj)));
        }


        auto curOp = CurOp::get(txn);

        // Get summary information about the plan.
        PlanSummaryStats stats;
        Explain::getSummaryStats(*executor.getValue(), &stats);
        if (collection) {
            collection->infoCache()->notifyOfQuery(txn, stats.indexesUsed);
        }
        curOp->debug().setPlanSummaryMetrics(stats);

        if (curOp->shouldDBProfile(curOp->elapsedMillis())) {
            BSONObjBuilder execStatsBob;
            Explain::getWinningPlanStats(executor.getValue().get(), &execStatsBob);
            curOp->debug().execStats = execStatsBob.obj();
        }

        verify(start == bb.buf());

        result.appendArray("values", arr.done());

        return true;
    }
} distinctCmd;

}  // namespace mongo
