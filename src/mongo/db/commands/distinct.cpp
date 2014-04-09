// distinct.cpp

/**
*    Copyright (C) 2012 10gen Inc.
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
#include "mongo/db/instance.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/kill_current_op.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/query/get_runner.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/db/query/type_explain.h"
#include "mongo/util/timer.h"

namespace mongo {

    class DistinctCommand : public Command {
    public:
        DistinctCommand() : Command("distinct") {}

        virtual bool slaveOk() const { return false; }
        virtual bool slaveOverrideOk() const { return true; }
        virtual bool isWriteCommandForConfigServer() const { return false; }

        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::find);
            out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
        }

        virtual void help( stringstream &help ) const {
            help << "{ distinct : 'collection name' , key : 'a.b' , query : {} }";
        }

        bool run(const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result,
                 bool fromRepl ) {

            Timer t;
            string ns = dbname + '.' + cmdObj.firstElement().valuestr();

            string key = cmdObj["key"].valuestrsafe();
            BSONObj keyPattern = BSON( key << 1 );

            BSONObj query = getQuery( cmdObj );

            int bufSize = BSONObjMaxUserSize - 4096;
            BufBuilder bb( bufSize );
            char * start = bb.buf();

            BSONArrayBuilder arr( bb );
            BSONElementSet values;

            long long nscanned = 0; // locations looked at
            long long nscannedObjects = 0; // full objects looked at
            long long n = 0; // matches

            Client::ReadContext ctx(ns);

            Collection* collection = cc().database()->getCollection( ns );

            if (!collection) {
                result.appendArray( "values" , BSONObj() );
                result.append("stats", BSON("n" << 0 <<
                                            "nscanned" << 0 <<
                                            "nscannedObjects" << 0));
                return true;
            }

            Runner* rawRunner;
            Status status = getRunnerDistinct(collection, query, key, &rawRunner);
            if (!status.isOK()) {
                uasserted(17216, mongoutils::str::stream() << "Can't get runner for query "
                              << query << ": " << status.toString());
                return 0;
            }

            auto_ptr<Runner> runner(rawRunner);
            const ScopedRunnerRegistration safety(runner.get());
            runner->setYieldPolicy(Runner::YIELD_AUTO);

            string cursorName;
            BSONObj obj;
            Runner::RunnerState state;
            while (Runner::RUNNER_ADVANCED == (state = runner->getNext(&obj, NULL))) {
                // Distinct expands arrays.
                //
                // If our query is covered, each value of the key should be in the index key and
                // available to us without this.  If a collection scan is providing the data, we may
                // have to expand an array.
                BSONElementSet elts;
                obj.getFieldsDotted(key, elts);

                for (BSONElementSet::iterator it = elts.begin(); it != elts.end(); ++it) {
                    BSONElement elt = *it;
                    if (values.count(elt)) { continue; }
                    int currentBufPos = bb.len();

                    uassert(17217, "distinct too big, 16mb cap",
                            (currentBufPos + elt.size() + 1024) < bufSize);

                    arr.append(elt);
                    BSONElement x(start + currentBufPos);
                    values.insert(x);
                }
            }
            TypeExplain* bareExplain;
            Status res = runner->getInfo(&bareExplain, NULL);
            if (res.isOK()) {
                auto_ptr<TypeExplain> explain(bareExplain);
                if (explain->isCursorSet()) {
                    cursorName = explain->getCursor();
                }
                n = explain->getN();
                nscanned = explain->getNScanned();
                nscannedObjects = explain->getNScannedObjects();
            }

            verify( start == bb.buf() );

            result.appendArray( "values" , arr.done() );

            {
                BSONObjBuilder b;
                b.appendNumber( "n" , n );
                b.appendNumber( "nscanned" , nscanned );
                b.appendNumber( "nscannedObjects" , nscannedObjects );
                b.appendNumber( "timems" , t.millis() );
                b.append( "cursor" , cursorName );
                result.append( "stats" , b.obj() );
            }

            return true;
        }
    } distinctCmd;

}  // namespace mongo
