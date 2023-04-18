/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include <memory>

#include "mongo/base/init.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/and_hash.h"
#include "mongo/db/exec/and_sorted.h"
#include "mongo/db/exec/collection_scan.h"
#include "mongo/db/exec/delete_stage.h"
#include "mongo/db/exec/fetch.h"
#include "mongo/db/exec/index_scan.h"
#include "mongo/db/exec/limit.h"
#include "mongo/db/exec/merge_sort.h"
#include "mongo/db/exec/or.h"
#include "mongo/db/exec/skip.h"
#include "mongo/db/exec/sort.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/index/fts_access_method.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/expression_text_base.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/plan_executor_factory.h"

namespace mongo {

using std::string;
using std::unique_ptr;
using std::vector;

/**
 * A command for manually constructing a query tree and running it.
 *
 * db.runCommand({stageDebug: {collection: collname, plan: rootNode}})
 *
 * The value of the filter field is a BSONObj that specifies values that fields must have.  What
 * you'd pass to a matcher.
 *
 * Leaf Nodes:
 *
 * node -> {ixscan: {filter: {FILTER},
 *                   args: {indexKeyPattern: kpObj, start: startObj,
 *                          stop: stopObj, endInclusive: true/false, direction: -1/1,
 *                          limit: int}}}
 * node -> {cscan: {filter: {filter}, args: {direction: -1/1}}}
 * TODO: language for text.
 * node -> {text: {filter: {filter}, args: {search: "searchstr"}}}
 *
 * Internal Nodes:
 *
 * node -> {andHash: {args: { nodes: [node, node]}}}
 * node -> {andSorted: {args: { nodes: [node, node]}}}
 * node -> {or: {filter: {filter}, args: { dedup:bool, nodes:[node, node]}}}
 * node -> {fetch: {filter: {filter}, args: {node: node}}}
 * node -> {limit: {args: {node: node, num: posint}}}
 * node -> {skip: {args: {node: node, num: posint}}}
 * node -> {sort: {args: {node: node, pattern: objWithSortCriterion }}}
 * node -> {mergeSort: {args: {nodes: [node, node], pattern: objWithSortCriterion}}}
 * node -> {delete: {args: {node: node, isMulti: bool}}}
 *
 * Forthcoming Nodes:
 *
 * node -> {dedup: {filter: {filter}, args: {node: node, field: field}}}
 * node -> {unwind: {filter: filter}, args: {node: node, field: field}}
 */
class StageDebugCmd : public BasicCommand {
public:
    StageDebugCmd() : BasicCommand("stageDebug") {}

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
    std::string help() const override {
        return {};
    }

    Status checkAuthForOperation(OperationContext*,
                                 const DatabaseName&,
                                 const BSONObj&) const override {
        // Command is testing-only, and can only be enabled at command line.
        //  Hence, no auth check needed.
        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        BSONElement argElt = cmdObj["stageDebug"];
        if (argElt.eoo() || !argElt.isABSONObj()) {
            return false;
        }
        BSONObj argObj = argElt.Obj();

        // Pull out the collection name.
        BSONElement collElt = argObj["collection"];
        if (collElt.eoo() || (String != collElt.type())) {
            return false;
        }

        const NamespaceString nss(
            NamespaceStringUtil::parseNamespaceFromRequest(dbName, collElt.String()));
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << nss.toStringForErrorMsg() << " is not a valid namespace",
                nss.isValid());

        auto expCtx = make_intrusive<ExpressionContext>(
            opCtx, std::unique_ptr<CollatorInterface>(nullptr), nss);

        // Need a context to get the actual const CollectionPtr&
        // TODO A write lock is currently taken here to accommodate stages that perform writes
        //      (e.g. DeleteStage).  This should be changed to use a read lock for read-only
        //      execution trees.
        AutoGetCollection autoColl(opCtx, nss, MODE_IX);

        // Make sure the collection is valid.
        const auto& collection = autoColl.getCollection();
        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "Couldn't find collection " << nss.toStringForErrorMsg(),
                collection);

        // Pull out the plan
        BSONElement planElt = argObj["plan"];
        if (planElt.eoo() || !planElt.isABSONObj()) {
            return false;
        }
        BSONObj planObj = planElt.Obj();

        // Parse the plan into these.
        std::vector<std::unique_ptr<MatchExpression>> exprs;
        unique_ptr<WorkingSet> ws(new WorkingSet());

        std::unique_ptr<PlanStage> userRoot{
            parseQuery(expCtx, collection, planObj, ws.get(), nss, &exprs)};
        uassert(16911, "Couldn't parse plan from " + cmdObj.toString(), nullptr != userRoot);

        // Add a fetch at the top for the user so we can get obj back for sure.
        unique_ptr<PlanStage> rootFetch = std::make_unique<FetchStage>(
            expCtx.get(), ws.get(), std::move(userRoot), nullptr, collection);

        auto statusWithPlanExecutor =
            plan_executor_factory::make(expCtx,
                                        std::move(ws),
                                        std::move(rootFetch),
                                        &collection,
                                        PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                        false /* whether owned BSON must be returned */);
        fassert(28536, statusWithPlanExecutor.getStatus());
        auto exec = std::move(statusWithPlanExecutor.getValue());

        BSONArrayBuilder resultBuilder(result.subarrayStart("results"));

        BSONObj obj;
        PlanExecutor::ExecState state;
        while (PlanExecutor::ADVANCED == (state = exec->getNext(&obj, nullptr))) {
            resultBuilder.append(obj);
        }

        resultBuilder.done();
        return true;
    }

    PlanStage* parseQuery(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                          const CollectionPtr& collection,
                          BSONObj obj,
                          WorkingSet* workingSet,
                          const NamespaceString& nss,
                          std::vector<std::unique_ptr<MatchExpression>>* exprs) {
        OperationContext* opCtx = expCtx->opCtx;

        BSONElement firstElt = obj.firstElement();
        if (!firstElt.isABSONObj()) {
            return nullptr;
        }
        BSONObj paramObj = firstElt.Obj();

        MatchExpression* matcher = nullptr;
        BSONObj nodeArgs;

        // Every node has these two fields.
        const string filterTag = "filter";
        const string argsTag = "args";

        BSONObjIterator it(paramObj);
        while (it.more()) {
            BSONElement e = it.next();
            if (!e.isABSONObj()) {
                return nullptr;
            }
            BSONObj argObj = e.Obj();
            if (filterTag == e.fieldName()) {
                auto statusWithMatcher =
                    MatchExpressionParser::parse(argObj,
                                                 expCtx,
                                                 ExtensionsCallbackReal(opCtx, &collection->ns()),
                                                 MatchExpressionParser::kAllowAllSpecialFeatures);
                if (!statusWithMatcher.isOK()) {
                    return nullptr;
                }
                std::unique_ptr<MatchExpression> me = std::move(statusWithMatcher.getValue());
                // exprs is what will wind up deleting this.
                matcher = me.get();
                verify(nullptr != matcher);
                exprs->push_back(std::move(me));
            } else if (argsTag == e.fieldName()) {
                nodeArgs = argObj;
            } else {
                uasserted(16910,
                          "Unknown fieldname " + string(e.fieldName()) + " in query node " +
                              obj.toString());
                return nullptr;
            }
        }

        string nodeName = firstElt.fieldName();

        if ("ixscan" == nodeName) {
            const IndexDescriptor* desc;
            if (BSONElement keyPatternElement = nodeArgs["keyPattern"]) {
                // This'll throw if it's not an obj but that's OK.
                BSONObj keyPatternObj = keyPatternElement.Obj();
                std::vector<const IndexDescriptor*> indexes;
                collection->getIndexCatalog()->findIndexesByKeyPattern(
                    opCtx, keyPatternObj, IndexCatalog::InclusionPolicy::kReady, &indexes);
                uassert(16890,
                        str::stream() << "Can't find index: " << keyPatternObj,
                        !indexes.empty());
                uassert(ErrorCodes::AmbiguousIndexKeyPattern,
                        str::stream() << indexes.size()
                                      << " matching indexes for key pattern: " << keyPatternObj
                                      << ". Conflicting indexes: " << indexes[0]->infoObj() << ", "
                                      << indexes[1]->infoObj(),
                        indexes.size() == 1);
                desc = indexes[0];
            } else {
                uassert(40306,
                        str::stream() << "Index 'name' must be a string in: " << nodeArgs,
                        nodeArgs["name"].type() == BSONType::String);
                StringData name = nodeArgs["name"].valueStringData();
                desc = collection->getIndexCatalog()->findIndexByName(opCtx, name);
                uassert(40223, str::stream() << "Can't find index: " << name.toString(), desc);
            }

            IndexScanParams params(opCtx, collection, desc);
            params.bounds.isSimpleRange = true;
            params.bounds.startKey = BSONObj::stripFieldNames(nodeArgs["startKey"].Obj());
            params.bounds.endKey = BSONObj::stripFieldNames(nodeArgs["endKey"].Obj());
            params.bounds.boundInclusion = IndexBounds::makeBoundInclusionFromBoundBools(
                nodeArgs["startKeyInclusive"].Bool(), nodeArgs["endKeyInclusive"].Bool());
            params.direction = nodeArgs["direction"].numberInt();
            params.shouldDedup = desc->getEntry()->isMultikey(opCtx, collection);

            return new IndexScan(expCtx.get(), collection, params, workingSet, matcher);
        } else if ("andHash" == nodeName) {
            uassert(
                16921, "Nodes argument must be provided to AND", nodeArgs["nodes"].isABSONObj());

            auto andStage = std::make_unique<AndHashStage>(expCtx.get(), workingSet);

            int nodesAdded = 0;
            BSONObjIterator it(nodeArgs["nodes"].Obj());
            while (it.more()) {
                BSONElement e = it.next();
                uassert(16922, "node of AND isn't an obj?: " + e.toString(), e.isABSONObj());

                std::unique_ptr<PlanStage> subNode{
                    parseQuery(expCtx, collection, e.Obj(), workingSet, nss, exprs)};
                uassert(16923,
                        "Can't parse sub-node of AND: " + e.Obj().toString(),
                        nullptr != subNode);
                andStage->addChild(std::move(subNode));
                ++nodesAdded;
            }

            uassert(16927, "AND requires more than one child", nodesAdded >= 2);

            return andStage.release();
        } else if ("andSorted" == nodeName) {
            uassert(
                16924, "Nodes argument must be provided to AND", nodeArgs["nodes"].isABSONObj());

            auto andStage = std::make_unique<AndSortedStage>(expCtx.get(), workingSet);

            int nodesAdded = 0;
            BSONObjIterator it(nodeArgs["nodes"].Obj());
            while (it.more()) {
                BSONElement e = it.next();
                uassert(16925, "node of AND isn't an obj?: " + e.toString(), e.isABSONObj());

                std::unique_ptr<PlanStage> subNode{
                    parseQuery(expCtx, collection, e.Obj(), workingSet, nss, exprs)};
                uassert(16926,
                        "Can't parse sub-node of AND: " + e.Obj().toString(),
                        nullptr != subNode);
                andStage->addChild(std::move(subNode));
                ++nodesAdded;
            }

            uassert(16928, "AND requires more than one child", nodesAdded >= 2);

            return andStage.release();
        } else if ("or" == nodeName) {
            uassert(
                16934, "Nodes argument must be provided to AND", nodeArgs["nodes"].isABSONObj());
            uassert(16935, "Dedup argument must be provided to OR", !nodeArgs["dedup"].eoo());
            BSONObjIterator it(nodeArgs["nodes"].Obj());
            auto orStage = std::make_unique<OrStage>(
                expCtx.get(), workingSet, nodeArgs["dedup"].Bool(), matcher);
            while (it.more()) {
                BSONElement e = it.next();
                if (!e.isABSONObj()) {
                    return nullptr;
                }
                std::unique_ptr<PlanStage> subNode{
                    parseQuery(expCtx, collection, e.Obj(), workingSet, nss, exprs)};
                uassert(
                    16936, "Can't parse sub-node of OR: " + e.Obj().toString(), nullptr != subNode);
                orStage->addChild(std::move(subNode));
            }

            return orStage.release();
        } else if ("fetch" == nodeName) {
            uassert(
                16929, "Node argument must be provided to fetch", nodeArgs["node"].isABSONObj());
            std::unique_ptr<PlanStage> subNode{
                parseQuery(expCtx, collection, nodeArgs["node"].Obj(), workingSet, nss, exprs)};
            uassert(28731,
                    "Can't parse sub-node of FETCH: " + nodeArgs["node"].Obj().toString(),
                    nullptr != subNode);
            return new FetchStage(
                expCtx.get(), workingSet, std::move(subNode), matcher, collection);
        } else if ("limit" == nodeName) {
            uassert(16937,
                    "Limit stage doesn't have a filter (put it on the child)",
                    nullptr == matcher);
            uassert(
                16930, "Node argument must be provided to limit", nodeArgs["node"].isABSONObj());
            uassert(16931, "Num argument must be provided to limit", nodeArgs["num"].isNumber());
            std::unique_ptr<PlanStage> subNode{
                parseQuery(expCtx, collection, nodeArgs["node"].Obj(), workingSet, nss, exprs)};
            uassert(28732,
                    "Can't parse sub-node of LIMIT: " + nodeArgs["node"].Obj().toString(),
                    nullptr != subNode);
            return new LimitStage(
                expCtx.get(), nodeArgs["num"].numberInt(), workingSet, std ::move(subNode));
        } else if ("skip" == nodeName) {
            uassert(16938,
                    "Skip stage doesn't have a filter (put it on the child)",
                    nullptr == matcher);
            uassert(16932, "Node argument must be provided to skip", nodeArgs["node"].isABSONObj());
            uassert(16933, "Num argument must be provided to skip", nodeArgs["num"].isNumber());
            std::unique_ptr<PlanStage> subNode{
                parseQuery(expCtx, collection, nodeArgs["node"].Obj(), workingSet, nss, exprs)};
            uassert(28733,
                    "Can't parse sub-node of SKIP: " + nodeArgs["node"].Obj().toString(),
                    nullptr != subNode);
            return new SkipStage(
                expCtx.get(), nodeArgs["num"].numberInt(), workingSet, std::move(subNode));
        } else if ("cscan" == nodeName) {
            CollectionScanParams params;

            // What direction?
            uassert(16963,
                    "Direction argument must be specified and be a number",
                    nodeArgs["direction"].isNumber());
            if (1 == nodeArgs["direction"].numberInt()) {
                params.direction = CollectionScanParams::FORWARD;
            } else {
                params.direction = CollectionScanParams::BACKWARD;
            }

            return new CollectionScan(expCtx.get(), collection, params, workingSet, matcher);
        } else if ("mergeSort" == nodeName) {
            uassert(
                16971, "Nodes argument must be provided to sort", nodeArgs["nodes"].isABSONObj());
            uassert(16972,
                    "Pattern argument must be provided to sort",
                    nodeArgs["pattern"].isABSONObj());

            MergeSortStageParams params;
            params.pattern = nodeArgs["pattern"].Obj();
            // Dedup is true by default.

            auto mergeStage = std::make_unique<MergeSortStage>(expCtx.get(), params, workingSet);

            BSONObjIterator it(nodeArgs["nodes"].Obj());
            while (it.more()) {
                BSONElement e = it.next();
                uassert(16973, "node of mergeSort isn't an obj?: " + e.toString(), e.isABSONObj());

                std::unique_ptr<PlanStage> subNode{
                    parseQuery(expCtx, collection, e.Obj(), workingSet, nss, exprs)};
                uassert(16974,
                        "Can't parse sub-node of mergeSort: " + e.Obj().toString(),
                        nullptr != subNode);
                mergeStage->addChild(std::move(subNode));
            }
            return mergeStage.release();
        } else if ("delete" == nodeName) {
            uassert(18636,
                    "Delete stage doesn't have a filter (put it on the child)",
                    nullptr == matcher);
            uassert(
                18637, "node argument must be provided to delete", nodeArgs["node"].isABSONObj());
            uassert(18638,
                    "isMulti argument must be provided to delete",
                    nodeArgs["isMulti"].type() == Bool);
            PlanStage* subNode =
                parseQuery(expCtx, collection, nodeArgs["node"].Obj(), workingSet, nss, exprs);
            uassert(28734,
                    "Can't parse sub-node of DELETE: " + nodeArgs["node"].Obj().toString(),
                    nullptr != subNode);
            auto params = std::make_unique<DeleteStageParams>();
            params->isMulti = nodeArgs["isMulti"].Bool();
            return new DeleteStage(
                expCtx.get(), std::move(params), workingSet, collection, subNode);
        } else {
            return nullptr;
        }
    }
};

MONGO_REGISTER_TEST_COMMAND(StageDebugCmd);

}  // namespace mongo
