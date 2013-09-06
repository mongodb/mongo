/**
 *    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/exec/and_hash.h"
#include "mongo/db/exec/and_sorted.h"
#include "mongo/db/exec/collection_scan.h"
#include "mongo/db/exec/fetch.h"
#include "mongo/db/exec/index_scan.h"
#include "mongo/db/exec/limit.h"
#include "mongo/db/exec/merge_sort.h"
#include "mongo/db/exec/or.h"
#include "mongo/db/exec/skip.h"
#include "mongo/db/exec/sort.h"
#include "mongo/db/index/catalog_hack.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/namespace_details.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/query/plan_executor.h"

namespace mongo {

    /**
     * A command for manually constructing a query tree and running it.
     *
     * db.runCommand({stageDebug: rootNode})
     *
     * The value of the filter field is a BSONObj that specifies values that fields must have.  What
     * you'd pass to a matcher.
     *
     * Leaf Nodes:
     *
     * node -> {ixscan: {filter: {FILTER},
     *                   args: {name: "collectionname", indexKeyPattern: kpObj, start: startObj,
     *                          stop: stopObj, endInclusive: true/false, direction: -1/1,
     *                          limit: int}}}
     * node -> {cscan: {filter: {filter}, args: {name: "collectionname", direction: -1/1}}}
     *
     * Internal Nodes:
     *
     * node -> {andHash: {filter: {filter}, args: { nodes: [node, node]}}}
     * node -> {andSorted: {filter: {filter}, args: { nodes: [node, node]}}}
     * node -> {or: {filter: {filter}, args: { dedup:bool, nodes:[node, node]}}}
     * node -> {fetch: {filter: {filter}, args: {node: node}}}
     * node -> {limit: {args: {node: node, num: posint}}}
     * node -> {skip: {args: {node: node, num: posint}}}
     * node -> {sort: {args: {node: node, pattern: objWithSortCriterion }}}
     * node -> {mergeSort: {args: {nodes: [node, node], pattern: objWithSortCriterion}}}
     * node -> {cscan: {filter: {filter}, args: {name: "collectionname" }}}
     *
     * Forthcoming Nodes:
     *
     * node -> {dedup: {filter: {filter}, args: {node: node, field: field}}}
     * node -> {unwind: {filter: filter}, args: {node: node, field: field}}
     */
    class StageDebugCmd : public Command {
    public:
        StageDebugCmd() : Command("stageDebug") { }

        // Boilerplate for commands
        virtual LockType locktype() const { return READ; }
        bool slaveOk() const { return true; }
        bool slaveOverrideOk() const { return true; }
        void help(stringstream& h) const { }

        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::find);
            out->push_back(Privilege(parseNs(dbname, cmdObj), actions));
        }

        bool run(const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result,
                 bool fromRepl) {

            BSONElement argElt = cmdObj["stageDebug"];
            if (argElt.eoo() || !argElt.isABSONObj()) { return false; }
            BSONObj argObj = argElt.Obj();

            OwnedPointerVector<MatchExpression> exprs;
            auto_ptr<WorkingSet> ws(new WorkingSet());

            PlanStage* userRoot = parseQuery(dbname, argObj, ws.get(), &exprs);
            uassert(16911, "Couldn't parse plan from " + argObj.toString(), NULL != userRoot);

            // Add a fetch at the top for the user so we can get obj back for sure.
            // TODO: Do we want to do this for the user?  I think so.
            PlanStage* rootFetch = new FetchStage(ws.get(), userRoot, NULL);

            PlanExecutor runner(ws.release(), rootFetch);

            BSONArrayBuilder resultBuilder(result.subarrayStart("results"));

            for (BSONObj obj; Runner::RUNNER_ADVANCED == runner.getNext(&obj, NULL); ) {
                resultBuilder.append(obj);
            }

            resultBuilder.done();
            return true;
        }

        PlanStage* parseQuery(const string& dbname, BSONObj obj, WorkingSet* workingSet,
                              OwnedPointerVector<MatchExpression>* exprs) {
            BSONElement firstElt = obj.firstElement();
            if (!firstElt.isABSONObj()) { return NULL; }
            BSONObj paramObj = firstElt.Obj();

            MatchExpression* matcher = NULL;
            BSONObj nodeArgs;

            // Every node has these two fields.
            const string filterTag = "filter";
            const string argsTag = "args";

            BSONObjIterator it(paramObj);
            while (it.more()) {
                BSONElement e = it.next();
                if (!e.isABSONObj()) { return NULL; }
                BSONObj argObj = e.Obj();
                if (filterTag == e.fieldName()) {
                    StatusWithMatchExpression swme = MatchExpressionParser::parse(argObj);
                    if (!swme.isOK()) { return NULL; }
                    // exprs is what will wind up deleting this.
                    matcher = swme.getValue();
                    verify(NULL != matcher);
                    exprs->mutableVector().push_back(matcher);
                }
                else if (argsTag == e.fieldName()) {
                    nodeArgs = argObj;
                }
                else {
                    uasserted(16910, "Unknown fieldname " + string(e.fieldName())
                                     + " in query node " + obj.toString());
                    return NULL;
                }
            }

            string nodeName = firstElt.fieldName();

            if ("ixscan" == nodeName) {
                NamespaceDetails* nsd = nsdetails(dbname + "." + nodeArgs["name"].String());
                uassert(16913, "Can't find collection " + nodeArgs["name"].String(), nsd);

                int idxNo = nsd->findIndexByKeyPattern(nodeArgs["keyPattern"].Obj());
                uassert(16890, "Can't find index: " + nodeArgs["keyPattern"].Obj().toString(),
                        idxNo != -1);

                IndexScanParams params;
                params.descriptor = CatalogHack::getDescriptor(nsd, idxNo);
                params.bounds.isSimpleRange = true;
                params.bounds.startKey = nodeArgs["startKey"].Obj();
                params.bounds.endKey = nodeArgs["endKey"].Obj();
                params.bounds.endKeyInclusive = nodeArgs["endKeyInclusive"].Bool();
                params.direction = nodeArgs["direction"].numberInt();
                params.limit = nodeArgs["limit"].numberInt();
                params.forceBtreeAccessMethod = false;

                return new IndexScan(params, workingSet, matcher);
            }
            else if ("andHash" == nodeName) {
                uassert(16921, "Nodes argument must be provided to AND",
                        nodeArgs["nodes"].isABSONObj());

                auto_ptr<AndHashStage> andStage(new AndHashStage(workingSet, matcher));

                int nodesAdded = 0;
                BSONObjIterator it(nodeArgs["nodes"].Obj());
                while (it.more()) {
                    BSONElement e = it.next();
                    uassert(16922, "node of AND isn't an obj?: " + e.toString(),
                            e.isABSONObj());

                    PlanStage* subNode = parseQuery(dbname, e.Obj(), workingSet, exprs);
                    uassert(16923, "Can't parse sub-node of AND: " + e.Obj().toString(),
                            NULL != subNode);
                    // takes ownership
                    andStage->addChild(subNode);
                    ++nodesAdded;
                }

                uassert(16927, "AND requires more than one child", nodesAdded >= 2);

                return andStage.release();
            }
            else if ("andSorted" == nodeName) {
                uassert(16924, "Nodes argument must be provided to AND",
                        nodeArgs["nodes"].isABSONObj());

                auto_ptr<AndSortedStage> andStage(new AndSortedStage(workingSet,
                                                                     matcher));

                int nodesAdded = 0;
                BSONObjIterator it(nodeArgs["nodes"].Obj());
                while (it.more()) {
                    BSONElement e = it.next();
                    uassert(16925, "node of AND isn't an obj?: " + e.toString(),
                            e.isABSONObj());

                    PlanStage* subNode = parseQuery(dbname, e.Obj(), workingSet, exprs);
                    uassert(16926, "Can't parse sub-node of AND: " + e.Obj().toString(),
                            NULL != subNode);
                    // takes ownership
                    andStage->addChild(subNode);
                    ++nodesAdded;
                }

                uassert(16928, "AND requires more than one child", nodesAdded >= 2);

                return andStage.release();
            }
            else if ("or" == nodeName) {
                uassert(16934, "Nodes argument must be provided to AND",
                        nodeArgs["nodes"].isABSONObj());
                uassert(16935, "Dedup argument must be provided to OR",
                        !nodeArgs["dedup"].eoo());
                BSONObjIterator it(nodeArgs["nodes"].Obj());
                auto_ptr<OrStage> orStage(new OrStage(workingSet, nodeArgs["dedup"].Bool(),
                                                      matcher));
                while (it.more()) {
                    BSONElement e = it.next();
                    if (!e.isABSONObj()) { return NULL; }
                    PlanStage* subNode = parseQuery(dbname, e.Obj(), workingSet, exprs);
                    uassert(16936, "Can't parse sub-node of OR: " + e.Obj().toString(),
                            NULL != subNode);
                    // takes ownership
                    orStage->addChild(subNode);
                }

                return orStage.release();
            }
            else if ("fetch" == nodeName) {
                uassert(16929, "Node argument must be provided to fetch",
                        nodeArgs["node"].isABSONObj());
                PlanStage* subNode = parseQuery(dbname, nodeArgs["node"].Obj(), workingSet, exprs);
                return new FetchStage(workingSet, subNode, matcher);
            }
            else if ("limit" == nodeName) {
                uassert(16937, "Limit stage doesn't have a filter (put it on the child)",
                        NULL == matcher);
                uassert(16930, "Node argument must be provided to limit",
                        nodeArgs["node"].isABSONObj());
                uassert(16931, "Num argument must be provided to limit",
                        nodeArgs["num"].isNumber());
                PlanStage* subNode = parseQuery(dbname, nodeArgs["node"].Obj(), workingSet, exprs);
                return new LimitStage(nodeArgs["num"].numberInt(), workingSet, subNode);
            }
            else if ("skip" == nodeName) {
                uassert(16938, "Skip stage doesn't have a filter (put it on the child)",
                        NULL == matcher);
                uassert(16932, "Node argument must be provided to skip",
                        nodeArgs["node"].isABSONObj());
                uassert(16933, "Num argument must be provided to skip",
                        nodeArgs["num"].isNumber());
                PlanStage* subNode = parseQuery(dbname, nodeArgs["node"].Obj(), workingSet, exprs);
                return new SkipStage(nodeArgs["num"].numberInt(), workingSet, subNode);
            }
            else if ("cscan" == nodeName) {
                CollectionScanParams params;

                // What collection?
                params.ns = dbname + "." + nodeArgs["name"].String();
                uassert(16962, "Can't find collection " + nodeArgs["name"].String(),
                        NULL != nsdetails(params.ns));

                // What direction?
                uassert(16963, "Direction argument must be specified and be a number",
                        nodeArgs["direction"].isNumber());
                if (1 == nodeArgs["direction"].numberInt()) {
                    params.direction = CollectionScanParams::FORWARD;
                }
                else {
                    params.direction = CollectionScanParams::BACKWARD;
                }

                return new CollectionScan(params, workingSet, matcher);
            }
            else if ("sort" == nodeName) {
                uassert(16969, "Node argument must be provided to sort",
                        nodeArgs["node"].isABSONObj());
                uassert(16970, "Pattern argument must be provided to sort",
                        nodeArgs["pattern"].isABSONObj());
                PlanStage* subNode = parseQuery(dbname, nodeArgs["node"].Obj(), workingSet, exprs);
                SortStageParams params;
                params.pattern = nodeArgs["pattern"].Obj();
                return new SortStage(params, workingSet, subNode);
            }
            else if ("mergeSort" == nodeName) {
                uassert(16971, "Nodes argument must be provided to sort",
                        nodeArgs["nodes"].isABSONObj());
                uassert(16972, "Pattern argument must be provided to sort",
                        nodeArgs["pattern"].isABSONObj());

                MergeSortStageParams params;
                params.pattern = nodeArgs["pattern"].Obj();
                // Dedup is true by default.

                auto_ptr<MergeSortStage> mergeStage(new MergeSortStage(params, workingSet));

                BSONObjIterator it(nodeArgs["nodes"].Obj());
                while (it.more()) {
                    BSONElement e = it.next();
                    uassert(16973, "node of mergeSort isn't an obj?: " + e.toString(),
                            e.isABSONObj());

                    PlanStage* subNode = parseQuery(dbname, e.Obj(), workingSet, exprs);
                    uassert(16974, "Can't parse sub-node of mergeSort: " + e.Obj().toString(),
                            NULL != subNode);
                    // takes ownership
                    mergeStage->addChild(subNode);
                }
                return mergeStage.release();
            }
            else {
                return NULL;
            }
        }
    } stageDebugCmd;

}  // namespace mongo
