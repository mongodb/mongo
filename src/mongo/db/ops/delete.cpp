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

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/namespace_details.h"
#include "mongo/db/query_optimizer.h"   // XXX old sys
#include "mongo/db/queryutil.h"   // XXX old sys
#include "mongo/db/query/new_find.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/structure/collection.h"
#include "mongo/util/stacktrace.h"


namespace mongo {

    MONGO_EXPORT_SERVER_PARAMETER(newDelete, bool, true);

    /* ns:      namespace, e.g. <database>.<collection>
       pattern: the "where" clause / criteria
       justOne: stop after 1 match
       god:     allow access to system namespaces, and don't yield
    */
    long long deleteObjects(const StringData& ns, BSONObj pattern, bool justOne, bool logop, bool god) {
        // cout << "enter delete, pattern is " << pattern.toString() << endl;

        if (!god) {
            if (ns.find( ".system.") != string::npos) {
                /* note a delete from system.indexes would corrupt the db
                if done here, as there are pointers into those objects in
                NamespaceDetails.
                */
                uassert(12050, "cannot delete from system namespace", legalClientSystemNS( ns, true ) );
            }

            if (ns.find('$') != string::npos) {
                log() << "cannot delete from collection with reserved $ in name: " << ns << endl;
                uasserted( 10100, "cannot delete from collection with reserved $ in name" );
            }
        }

        {
            NamespaceDetails *d = nsdetails( ns );
            if (NULL == d) {
                return 0;
            }
            uassert(10101, "can't remove from a capped collection", !d->isCapped());
        }

        string nsForLogOp = ns.toString(); // XXX-ERH

        long long nDeleted = 0;

        if (newDelete) {
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

            // cout << "yielding?: " << canYield << endl;

            if (canYield) {
                ClientCursor::registerRunner(runner.get());
                runner->setYieldPolicy(Runner::YIELD_AUTO);
                safety.reset(new DeregisterEvenIfUnderlyingCodeThrows(runner.get()));
            }

            // XXX: when will we not get a loc back?  fix invalidation to drop results instead
            // of fetching them...need to assume we have a diskloc...
            DiskLoc rloc;
            Runner::RunnerState state;
            while (Runner::RUNNER_ADVANCED == (state = runner->getNext(NULL, &rloc))) {
                if (logop) {
                    BSONElement idElt;
                    if (BSONObj::make(rloc.rec()).getObjectID(idElt)) {
                        BSONObjBuilder bob;
                        bob.append(idElt);
                        bool replJustOne = true;
                        logOp("d", nsForLogOp.c_str(), bob.done(), 0, &replJustOne);
                    }
                    else {
                        problem() << "deleted object without id, not logging" << endl;
                    }
                }

                // XXX: do we want to buffer docs and delete them in a group rather than
                // saving/restoring state repeatedly?
                runner->saveState();
                Collection* collection = currentClient.get()->database()->getCollection(ns);
                verify( collection );
                collection->deleteDocument(rloc);
                runner->restoreState();

                nDeleted++;

                if (justOne) { break; }

                if (!god) {
                    getDur().commitIfNeeded();
                }

                if (debug && god && nDeleted == 100) {
                    // TODO: why does this use significant memory??
                    log() << "warning high number of deletes with god=true which could use significant memory" << endl;
                }
            }
        }
        else {
            shared_ptr< Cursor > creal = getOptimizedCursor( ns, pattern );

            if( !creal->ok() )
                return nDeleted;

            shared_ptr< Cursor > cPtr = creal;
            auto_ptr<ClientCursor> cc( new ClientCursor( QueryOption_NoCursorTimeout, cPtr, ns) );
            cc->setDoingDeletes( true );

            CursorId id = cc->cursorid();

            bool canYield = !god && !(creal->matcher() && creal->matcher()->docMatcher().atomic());

            do {
                // TODO: we can generalize this I believe
                //       
                bool willNeedRecord = (creal->matcher() && creal->matcher()->needRecord()) || pattern.isEmpty() || isSimpleIdQuery( pattern );
                if ( ! willNeedRecord ) {
                    // TODO: this is a total hack right now
                    // check if the index full encompasses query

                    if ( pattern.nFields() == 1 && 
                            str::equals( pattern.firstElement().fieldName() , creal->indexKeyPattern().firstElement().fieldName() ) )
                        willNeedRecord = true;
                }

                if ( canYield && ! cc->yieldSometimes( willNeedRecord ? ClientCursor::WillNeed : ClientCursor::MaybeCovered ) ) {
                    cc.release(); // has already been deleted elsewhere
                    // TODO should we assert or something?
                    break;
                }
                if ( !cc->ok() ) {
                    break; // if we yielded, could have hit the end
                }

                // this way we can avoid calling prepareToYield() every time (expensive)
                // as well as some other nuances handled
                cc->setDoingDeletes( true );

                DiskLoc rloc = cc->currLoc();
                BSONObj key = cc->currKey();

                bool match = creal->currentMatches();

                cc->advance();

                if ( ! match )
                    continue;

                // SERVER-5198 Advance past the document to be modified, but see SERVER-5725.
                while( cc->ok() && rloc == cc->currLoc() ) {
                    cc->advance();
                }

                bool foundAllResults = ( justOne || !cc->ok() );

                if ( !foundAllResults ) {
                    // NOTE: Saving and restoring a btree cursor's position was historically described
                    // as slow here.
                    cc->c()->prepareToTouchEarlierIterate();
                }

                if ( logop ) {
                    BSONElement e;
                    if( BSONObj::make( rloc.rec() ).getObjectID( e ) ) {
                        BSONObjBuilder b;
                        b.append( e );
                        bool replJustOne = true;
                        logOp( "d", nsForLogOp.c_str(), b.done(), 0, &replJustOne );
                    }
                    else {
                        problem() << "deleted object without id, not logging" << endl;
                    }
                }

                currentClient.get()->database()->getCollection( ns )->deleteDocument( rloc );

                nDeleted++;
                if ( foundAllResults ) {
                    break;
                }
                cc->c()->recoverFromTouchingEarlierIterate();

                if( !god ) 
                    getDur().commitIfNeeded();

                if( debug && god && nDeleted == 100 ) 
                    log() << "warning high number of deletes with god=true which could use significant memory" << endl;
            }
            while ( cc->ok() );

            if ( cc.get() && ClientCursor::find( id , false ) == 0 ) {
                // TODO: remove this and the id declaration above if this doesn't trigger
                //       if it does, then i'm very confused (ERH 06/2011)
                error() << "this should be impossible" << endl;
                printStackTrace();
                cc.release();
            }
        }

        // cout << "ndeleted is " << nDeleted << endl;
        return nDeleted;
    }
}
