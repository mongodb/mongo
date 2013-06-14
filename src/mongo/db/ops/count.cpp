// count.cpp

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
 */

#include "mongo/db/ops/count.h"

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/query_optimizer.h"
#include "mongo/db/queryutil.h"
#include "mongo/util/elapsed_tracker.h"

namespace mongo {

    namespace {

        /**
         * Specialized Cursor creation rules that the count operator provides to the query
         * processing system.  These rules limit the performance overhead when counting index keys
         * matching simple predicates.  See SERVER-1752.
         */
        class CountPlanPolicies : public QueryPlanSelectionPolicy {

            virtual string name() const { return "CountPlanPolicies"; }

            virtual bool requestMatcher() const {
                // Avoid using a Matcher when a Cursor will exactly match a query.
                return false;
            }

            virtual bool requestIntervalCursor() const {
                // Request use of an IntervalBtreeCursor when the index bounds represent a single
                // btree interval.  This Cursor implementation is optimized for performing counts
                // between two endpoints.
                return true;
            }

        } _countPlanPolicies;

    }
    
    long long runCount( const char *ns, const BSONObj &cmd, string &err, int &errCode ) {
        Client::Context cx(ns);
        NamespaceDetails *d = nsdetails( ns );
        if ( !d ) {
            err = "ns missing";
            return -1;
        }
        BSONObj query = cmd.getObjectField("query");
        
        // count of all objects
        if ( query.isEmpty() ) {
            return applySkipLimit( d->numRecords(), cmd );
        }
        
        long long count = 0;
        long long skip = cmd["skip"].numberLong();
        long long limit = cmd["limit"].numberLong();

        if( limit < 0 ){
            limit  = -limit;
        }

        shared_ptr<Cursor> cursor = getOptimizedCursor( ns, query, BSONObj(), _countPlanPolicies );
        ClientCursor::Holder ccPointer;
        ElapsedTracker timeToStartYielding( 256, 20 );
        try {
            while( cursor->ok() ) {
                if ( !ccPointer ) {
                    if ( timeToStartYielding.intervalHasElapsed() ) {
                        // Lazily construct a ClientCursor, avoiding a performance regression when scanning a very
                        // small number of documents.
                        ccPointer.reset( new ClientCursor( QueryOption_NoCursorTimeout, cursor, ns ) );
                    }
                }
                else if ( !ccPointer->yieldSometimes( ClientCursor::MaybeCovered ) ||
                         !cursor->ok() ) {
                    break;
                }
                
                if ( cursor->currentMatches() && !cursor->getsetdup( cursor->currLoc() ) ) {
                    
                    if ( skip > 0 ) {
                        --skip;
                    }
                    else {
                        ++count;
                        if ( limit > 0 && count >= limit ) {
                            break;
                        }
                    }
                }
                cursor->advance();
            }
            ccPointer.reset();
            return count;
            
        }
        catch ( const DBException &e ) {
            err = e.toString();
            errCode = e.getCode();
        } 
        catch ( const std::exception &e ) {
            err = e.what();
            errCode = 0;
        } 
        // Historically we have returned zero in many count assertion cases - see SERVER-2291.
        log() << "Count with ns: " << ns << " and query: " << query
              << " failed with exception: " << err << " code: " << errCode
              << endl;
        return -2;
    }
    
} // namespace mongo
