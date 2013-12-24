/**
 *    Copyright (C) 2008 10gen Inc.
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

#include "mongo/db/ops/delete.h"

#include "mongo/db/clientcursor.h"
#include "mongo/db/namespace_details.h"
#include "mongo/db/query/get_runner.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/structure/collection.h"


namespace mongo {

    /* ns:      namespace, e.g. <database>.<collection>
       pattern: the "where" clause / criteria
       justOne: stop after 1 match
       god:     allow access to system namespaces, and don't yield
    */
    long long deleteObjects(const StringData& ns, BSONObj pattern, bool justOne, bool logop, bool god) {
        if (!god) {
            if (ns.find( ".system.") != string::npos) {
                // note a delete from system.indexes would corrupt the db if done here, as there are
                // pointers into those objects in NamespaceDetails.
                uassert(12050, "cannot delete from system namespace", legalClientSystemNS( ns, true ) );
            }

            if (ns.find('$') != string::npos) {
                log() << "cannot delete from collection with reserved $ in name: " << ns << endl;
                uasserted( 10100, "cannot delete from collection with reserved $ in name" );
            }
        }

        Collection* collection = currentClient.get()->database()->getCollection(ns);
        if (NULL == collection) {
            return 0;
        }

        uassert(10101,
                str::stream() << "can't remove from a capped collection: " << ns,
                !collection->isCapped());

        string nsForLogOp = ns.toString(); // XXX-ERH

        long long nDeleted = 0;

        CanonicalQuery* cq;
        if (!CanonicalQuery::canonicalize(ns.toString(), pattern, &cq).isOK()) {
            uasserted(17218, "Can't canonicalize query " + pattern.toString());
            return 0;
        }

        bool canYield = !god && !QueryPlannerCommon::hasNode(cq->root(), MatchExpression::ATOMIC);

        Runner* rawRunner;
        if (!getRunner(cq, &rawRunner).isOK()) {
            uasserted(17219, "Can't get runner for query " + pattern.toString());
            return 0;
        }

        auto_ptr<Runner> runner(rawRunner);
        auto_ptr<DeregisterEvenIfUnderlyingCodeThrows> safety;

        if (canYield) {
            ClientCursor::registerRunner(runner.get());
            runner->setYieldPolicy(Runner::YIELD_AUTO);
            safety.reset(new DeregisterEvenIfUnderlyingCodeThrows(runner.get()));
        }

        DiskLoc rloc;
        Runner::RunnerState state;
        while (Runner::RUNNER_ADVANCED == (state = runner->getNext(NULL, &rloc))) {

            BSONObj toDelete;

            // XXX: do we want to buffer docs and delete them in a group rather than
            // saving/restoring state repeatedly?
            runner->saveState();
            collection->deleteDocument(rloc, false, false, logop ? &toDelete : NULL );
            runner->restoreState();

            nDeleted++;

            if (logop) {
                if ( toDelete.isEmpty() ) {
                    problem() << "deleted object without id, not logging" << endl;
                }
                else {
                    bool replJustOne = true;
                    logOp("d", nsForLogOp.c_str(), toDelete, 0, &replJustOne);
                }
            }

            if (justOne) {
                break;
            }

            if (!god) {
                getDur().commitIfNeeded();
            }

            if (debug && god && nDeleted == 100) {
                log() << "warning high number of deletes with god=true "
                      << " which could use significant memory b/c we don't commit journal";
            }
        }

        return nDeleted;
    }
}
