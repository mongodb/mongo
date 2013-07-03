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
 */

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/exec/and_hash.h"
#include "mongo/db/exec/and_sorted.h"
#include "mongo/db/exec/index_scan.h"
#include "mongo/db/exec/simple_plan_runner.h"
#include "mongo/db/index/catalog_hack.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/namespace_details.h"
#include "mongo/db/namespace-inl.h"
#include "mongo/db/pdfile.h"

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
     *
     * Internal Nodes:
     *
     * node -> {andHash: {filter: {filter}, args: { nodes: [node, node]}}}
     * node -> {andSorted: {filter: {filter}, args: { nodes: [node, node]}}}
     *
     * Forthcoming Nodes:
     *
     * node -> {cscan: {filter: {filter}, args: {name: "collectionname" }}}
     * node -> {or: {filter: {filter}, args: { dedup:bool, nodes:[node, node]}}}
     * node -> {fetch: {filter: {filter}, args: {node: node}}}
     * node -> {sort: {filter: {filter}, args: {node: node, pattern: objWithSortCriterion}}}
     * node -> {dedup: {filter: {filter}, args: {node: node, field: field}}}
     * node -> {limit: {filter: filter}, args: {node: node, num: posint}}
     * node -> {skip: {filter: filter}, args: {node: node, num: posint}}
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

            SimplePlanRunner runner;
            auto_ptr<PlanStage> root(parseQuery(dbname, argObj, runner.getWorkingSet()));
            uassert(16911, "Couldn't parse plan from " + argObj.toString(), root.get());
            runner.setRoot(root.release());

            BSONArrayBuilder resultBuilder(result.subarrayStart("results"));

            for (BSONObj obj; runner.getNext(&obj); ) {
                resultBuilder.append(obj);
            }

            resultBuilder.done();
            return true;
        }

        PlanStage* parseQuery(const string& dbname, BSONObj obj, WorkingSet* workingSet) {
            BSONElement firstElt = obj.firstElement();
            if (!firstElt.isABSONObj()) { return NULL; }
            BSONObj paramObj = firstElt.Obj();

            auto_ptr<Matcher> matcher;
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
                    matcher.reset(new Matcher2(argObj));
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
                params.startKey = nodeArgs["startKey"].Obj();
                params.endKey = nodeArgs["endKey"].Obj();
                params.endKeyInclusive = nodeArgs["endKeyInclusive"].Bool();
                params.direction = nodeArgs["direction"].numberInt();
                params.limit = nodeArgs["limit"].numberInt();
                params.forceBtreeAccessMethod = false;

                return new IndexScan(params, workingSet, matcher.release());
            }
            else if ("andHash" == nodeName) {
                uassert(16921, "Nodes argument must be provided to AND",
                        nodeArgs["nodes"].isABSONObj());

                auto_ptr<AndHashStage> andStage(new AndHashStage(workingSet, matcher.release()));

                int nodesAdded = 0;
                BSONObjIterator it(nodeArgs["nodes"].Obj());
                while (it.more()) {
                    BSONElement e = it.next();
                    uassert(16922, "node of AND isn't an obj?: " + e.toString(),
                            e.isABSONObj());

                    PlanStage* subNode = parseQuery(dbname, e.Obj(), workingSet);
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
                                                                     matcher.release()));

                int nodesAdded = 0;
                BSONObjIterator it(nodeArgs["nodes"].Obj());
                while (it.more()) {
                    BSONElement e = it.next();
                    uassert(16925, "node of AND isn't an obj?: " + e.toString(),
                            e.isABSONObj());

                    PlanStage* subNode = parseQuery(dbname, e.Obj(), workingSet);
                    uassert(16926, "Can't parse sub-node of AND: " + e.Obj().toString(),
                            NULL != subNode);
                    // takes ownership
                    andStage->addChild(subNode);
                    ++nodesAdded;
                }

                uassert(16928, "AND requires more than one child", nodesAdded >= 2);

                return andStage.release();
            }
            else {
                return NULL;
            }
        }
    } stageDebugCmd;

}  // namespace mongo
