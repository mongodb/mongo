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

#include <string>
#include <vector>

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/instance.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/util/timer.h"

namespace mongo {

using std::unique_ptr;
using std::string;
using std::stringstream;

class DistinctCommand : public Command {
public:
    DistinctCommand() : Command("distinct") {}

    virtual bool slaveOk() const {
        return false;
    }
    virtual bool slaveOverrideOk() const {
        return true;
    }
    virtual bool isWriteCommandForConfigServer() const {
        return false;
    }
    bool supportsReadMajority() const final {
        return true;
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

    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& cmdObj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        Timer t;

        // ensure that the key is a string
        uassert(18510,
                mongoutils::str::stream() << "The first argument to the distinct command "
                                          << "must be a string but was a "
                                          << typeName(cmdObj["key"].type()),
                cmdObj["key"].type() == mongo::String);

        // ensure that the where clause is a document
        if (cmdObj["query"].isNull() == false && cmdObj["query"].eoo() == false) {
            uassert(18511,
                    mongoutils::str::stream() << "The query for the distinct command must be a "
                                              << "document but was a "
                                              << typeName(cmdObj["query"].type()),
                    cmdObj["query"].type() == mongo::Object);
        }

        string key = cmdObj["key"].valuestrsafe();
        BSONObj keyPattern = BSON(key << 1);

        BSONObj query = getQuery(cmdObj);

        int bufSize = BSONObjMaxUserSize - 4096;
        BufBuilder bb(bufSize);
        char* start = bb.buf();

        BSONArrayBuilder arr(bb);
        BSONElementSet values;

        const string ns = parseNs(dbname, cmdObj);
        AutoGetCollectionForRead ctx(txn, ns);

        Collection* collection = ctx.getCollection();
        if (!collection) {
            result.appendArray("values", BSONObj());
            result.append("stats", BSON("n" << 0 << "nscanned" << 0 << "nscannedObjects" << 0));
            return true;
        }

        auto statusWithPlanExecutor =
            getExecutorDistinct(txn, collection, query, key, PlanExecutor::YIELD_AUTO);
        if (!statusWithPlanExecutor.isOK()) {
            uasserted(17216,
                      mongoutils::str::stream() << "Can't get executor for query " << query << ": "
                                                << statusWithPlanExecutor.getStatus().toString());
            return 0;
        }

        unique_ptr<PlanExecutor> exec = std::move(statusWithPlanExecutor.getValue());

        BSONObj obj;
        PlanExecutor::ExecState state;
        while (PlanExecutor::ADVANCED == (state = exec->getNext(&obj, NULL))) {
            // Distinct expands arrays.
            //
            // If our query is covered, each value of the key should be in the index key and
            // available to us without this.  If a collection scan is providing the data, we may
            // have to expand an array.
            BSONElementSet elts;
            obj.getFieldsDotted(key, elts);

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

        // Get summary information about the plan.
        PlanSummaryStats stats;
        Explain::getSummaryStats(*exec, &stats);

        verify(start == bb.buf());

        result.appendArray("values", arr.done());

        {
            BSONObjBuilder b;
            b.appendNumber("n", stats.nReturned);
            b.appendNumber("nscanned", stats.totalKeysExamined);
            b.appendNumber("nscannedObjects", stats.totalDocsExamined);
            b.appendNumber("timems", t.millis());
            b.append("planSummary", Explain::getPlanSummary(exec.get()));
            result.append("stats", b.obj());
        }

        return true;
    }
} distinctCmd;

}  // namespace mongo
