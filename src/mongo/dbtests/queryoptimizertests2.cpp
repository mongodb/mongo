/**
 *    Copyright (C) 2009 10gen Inc.
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

#include "mongo/pch.h"

#include "mongo/db/query_optimizer_internal.h"

#include "mongo/db/instance.h"
#include "mongo/db/json.h"
#include "mongo/db/ops/count.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/ops/query.h"
#include "mongo/db/query_optimizer.h"
#include "mongo/db/queryutil.h"
#include "mongo/dbtests/dbtests.h"


namespace mongo {
    extern void runQuery(Message& m, QueryMessage& q, Message &response );
} // namespace mongo

namespace {

    using boost::shared_ptr;
    
    void dropCollection( const char *ns ) {
     	string errmsg;
        BSONObjBuilder result;
        dropCollection( ns, errmsg, result );
    }
    
    namespace QueryPlanSetTests {

        class Base {
        public:
            Base() : _context( ns() ) {
                string err;
                userCreateNS( ns(), BSONObj(), err, false );
            }
            virtual ~Base() {
                if ( !nsd() )
                    return;
                NamespaceDetailsTransient::get_inlock( ns() ).clearQueryCache();
                dropCollection( ns() );
            }
        protected:
            static void assembleRequest( const string &ns, BSONObj query, int nToReturn, int nToSkip, BSONObj *fieldsToReturn, int queryOptions, Message &toSend ) {
                // see query.h for the protocol we are using here.
                BufBuilder b;
                int opts = queryOptions;
                b.appendNum(opts);
                b.appendStr(ns);
                b.appendNum(nToSkip);
                b.appendNum(nToReturn);
                query.appendSelfToBufBuilder(b);
                if ( fieldsToReturn )
                    fieldsToReturn->appendSelfToBufBuilder(b);
                toSend.setData(dbQuery, b.buf(), b.len());
            }
            QueryPattern makePattern( const BSONObj &query, const BSONObj &order ) {
                FieldRangeSet frs( ns(), query, true, true );
                return QueryPattern( frs, order );
            }
            shared_ptr<QueryPlanSet> makeQps( const BSONObj& query = BSONObj(),
                                              const BSONObj& order = BSONObj(),
                                              const BSONObj& hint = BSONObj(),
                                              bool allowSpecial = true ) {
                auto_ptr<FieldRangeSetPair> frsp( new FieldRangeSetPair( ns(), query ) );
                auto_ptr<FieldRangeSetPair> frspOrig( new FieldRangeSetPair( *frsp ) );
                return shared_ptr<QueryPlanSet>
                        ( QueryPlanSet::make( ns(), frsp, frspOrig, query, order,
                                              shared_ptr<const ParsedQuery>(), hint,
                                              QueryPlanGenerator::Use, BSONObj(), BSONObj(),
                                              allowSpecial ) );
            }
            static const char *ns() { return "unittests.QueryPlanSetTests"; }
            static NamespaceDetails *nsd() { return nsdetails( ns() ); }
            DBDirectClient &client() { return _client; }
        private:
            Lock::GlobalWrite lk_;
            Client::Context _context;
            DBDirectClient _client;
        };

        class ToString : public Base {
        public:
            void run() {
                // Just test that we don't crash.
                makeQps( BSON( "a" << 1 ) )->toString();
            }
        };
        
        class NoIndexes : public Base {
        public:
            void run() {
                ASSERT_EQUALS( 1, makeQps( BSON( "a" << 4 ), BSON( "b" << 1 ) )->nPlans() );
            }
        };

        class Optimal : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), false, "a_1" );
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), false, "b_2" );
                BSONObj query = BSON( "a" << 4 );

                // Only one optimal plan is added to the plan set.
                ASSERT_EQUALS( 1, makeQps( query )->nPlans() );

                // The optimal plan is recorded in the plan cache.
                FieldRangeSet frs( ns(), query, true, true );
                CachedQueryPlan cachedPlan =
                        NamespaceDetailsTransient::get( ns() ).cachedQueryPlanForPattern
                            ( QueryPattern( frs, BSONObj() ) );
                ASSERT_EQUALS( BSON( "a" << 1 ), cachedPlan.indexKey() );
                CandidatePlanCharacter planCharacter = cachedPlan.planCharacter();
                ASSERT( planCharacter.mayRunInOrderPlan() );
                ASSERT( !planCharacter.mayRunOutOfOrderPlan() );
            }
        };

        class NoOptimal : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), false, "a_1" );
                Helpers::ensureIndex( ns(), BSON( "b" << 1 ), false, "b_1" );
                ASSERT_EQUALS( 3, makeQps( BSON( "a" << 4 ), BSON( "b" << 1 ) )->nPlans() );
            }
        };

        class NoSpec : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), false, "a_1" );
                Helpers::ensureIndex( ns(), BSON( "b" << 1 ), false, "b_1" );
                ASSERT_EQUALS( 1, makeQps()->nPlans() );
            }
        };

        class HintSpec : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), false, "a_1" );
                Helpers::ensureIndex( ns(), BSON( "b" << 1 ), false, "b_1" );
                ASSERT_EQUALS( 1, makeQps( BSON( "a" << 1 ), BSON( "b" << 1 ),
                                           BSON( "hint" << BSON( "a" << 1 ) ) )->nPlans() );
            }
        };

        class HintName : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), false, "a_1" );
                Helpers::ensureIndex( ns(), BSON( "b" << 1 ), false, "b_1" );
                ASSERT_EQUALS( 1, makeQps( BSON( "a" << 1 ), BSON( "b" << 1 ),
                                           BSON( "hint" << "a_1" ) )->nPlans() );
            }
        };

        class NaturalHint : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), false, "a_1" );
                Helpers::ensureIndex( ns(), BSON( "b" << 1 ), false, "b_1" );
                ASSERT_EQUALS( 1, makeQps( BSON( "a" << 1 ), BSON( "b" << 1 ),
                                           BSON( "hint" << BSON( "$natural" << 1 ) ) )->nPlans() );
            }
        };

        class NaturalSort : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), false, "a_1" );
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), false, "b_2" );
                ASSERT_EQUALS( 1, makeQps( BSON( "a" << 1 ), BSON( "$natural" << 1 ) )->nPlans() );
            }
        };

        class BadHint : public Base {
        public:
            void run() {
                ASSERT_THROWS( makeQps( BSON( "a" << 1 ), BSON( "b" << 1 ),
                                        BSON( "hint" << "a_1" ) ),
                              AssertionException );
            }
        };

        class Count : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), false, "a_1" );
                Helpers::ensureIndex( ns(), BSON( "b" << 1 ), false, "b_1" );
                string err;
                int errCode;
                ASSERT_EQUALS( 0, runCount( ns(), BSON( "query" << BSON( "a" << 4 ) ), err, errCode ) );
                BSONObj one = BSON( "a" << 1 );
                BSONObj fourA = BSON( "a" << 4 );
                BSONObj fourB = BSON( "a" << 4 );
                theDataFileMgr.insertWithObjMod( ns(), one );
                ASSERT_EQUALS( 0, runCount( ns(), BSON( "query" << BSON( "a" << 4 ) ), err, errCode ) );
                theDataFileMgr.insertWithObjMod( ns(), fourA );
                ASSERT_EQUALS( 1, runCount( ns(), BSON( "query" << BSON( "a" << 4 ) ), err, errCode ) );
                theDataFileMgr.insertWithObjMod( ns(), fourB );
                ASSERT_EQUALS( 2, runCount( ns(), BSON( "query" << BSON( "a" << 4 ) ), err, errCode ) );
                ASSERT_EQUALS( 3, runCount( ns(), BSON( "query" << BSONObj() ), err, errCode ) );
                ASSERT_EQUALS( 3, runCount( ns(), BSON( "query" << BSON( "a" << GT << 0 ) ), err, errCode ) );
                // missing ns
                ASSERT_EQUALS( -1, runCount( "unittests.missingNS", BSONObj(), err, errCode ) );
                // impossible match
                ASSERT_EQUALS( 0, runCount( ns(), BSON( "query" << BSON( "a" << GT << 0 << LT << -1 ) ), err, errCode ) );
            }
        };

        class QueryMissingNs : public Base {
        public:
            QueryMissingNs() { mongo::unittest::log() << "querymissingns starts" << endl; }
            ~QueryMissingNs() {
                mongo::unittest::log() << "end QueryMissingNs" << endl;
            }
            void run() {
                Message m;
                assembleRequest( "unittests.missingNS", BSONObj(), 0, 0, 0, 0, m );
                DbMessage d(m);
                QueryMessage q(d);
                Message ret;
                runQuery( m, q, ret );
                ASSERT_EQUALS( 0, ((QueryResult*)ret.header())->nReturned );
            }

        };

        class UnhelpfulIndex : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), false, "a_1" );
                Helpers::ensureIndex( ns(), BSON( "b" << 1 ), false, "b_1" );
                ASSERT_EQUALS( 2, makeQps( BSON( "a" << 1 << "c" << 2 ) )->nPlans() );
            }
        };

        class FindOne : public Base {
        public:
            void run() {
                BSONObj one = BSON( "a" << 1 );
                theDataFileMgr.insertWithObjMod( ns(), one );
                BSONObj result;
                ASSERT( Helpers::findOne( ns(), BSON( "a" << 1 ), result ) );
                ASSERT_THROWS( Helpers::findOne( ns(), BSON( "a" << 1 ), result, true ), AssertionException );
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), false, "a_1" );
                ASSERT( Helpers::findOne( ns(), BSON( "a" << 1 ), result, true ) );
            }
        };

        class Delete : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), false, "a_1" );
                for( int i = 0; i < 200; ++i ) {
                    BSONObj two = BSON( "a" << 2 );
                    theDataFileMgr.insertWithObjMod( ns(), two );
                }
                BSONObj one = BSON( "a" << 1 );
                theDataFileMgr.insertWithObjMod( ns(), one );
                BSONObj delSpec = BSON( "a" << 1 << "_id" << NE << 0 );
                deleteObjects( ns(), delSpec, false );
                
                NamespaceDetailsTransient &nsdt = NamespaceDetailsTransient::get( ns() );
                QueryPattern queryPattern = FieldRangeSet( ns(), delSpec, true, true ).pattern();
                CachedQueryPlan cachedQueryPlan = nsdt.cachedQueryPlanForPattern( queryPattern ); 
                ASSERT_EQUALS( BSON( "a" << 1 ), cachedQueryPlan.indexKey() );
                ASSERT_EQUALS( 1, cachedQueryPlan.nScanned() );
            }
        };

        class DeleteOneScan : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "_id" << 1 ), false, "_id_1" );
                BSONObj one = BSON( "_id" << 3 << "a" << 1 );
                BSONObj two = BSON( "_id" << 2 << "a" << 1 );
                BSONObj three = BSON( "_id" << 1 << "a" << -1 );
                theDataFileMgr.insertWithObjMod( ns(), one );
                theDataFileMgr.insertWithObjMod( ns(), two );
                theDataFileMgr.insertWithObjMod( ns(), three );
                deleteObjects( ns(), BSON( "_id" << GT << 0 << "a" << GT << 0 ), true );
                for( boost::shared_ptr<Cursor> c = theDataFileMgr.findAll( ns() ); c->ok(); c->advance() )
                    ASSERT( 3 != c->current().getIntField( "_id" ) );
            }
        };

        class DeleteOneIndex : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), false, "a" );
                BSONObj one = BSON( "a" << 2 << "_id" << 0 );
                BSONObj two = BSON( "a" << 1 << "_id" << 1 );
                BSONObj three = BSON( "a" << 0 << "_id" << 2 );
                theDataFileMgr.insertWithObjMod( ns(), one );
                theDataFileMgr.insertWithObjMod( ns(), two );
                theDataFileMgr.insertWithObjMod( ns(), three );
                deleteObjects( ns(), BSON( "a" << GTE << 0 ), true );
                for( boost::shared_ptr<Cursor> c = theDataFileMgr.findAll( ns() ); c->ok(); c->advance() )
                    ASSERT( 2 != c->current().getIntField( "_id" ) );
            }
        };

        class InQueryIntervals : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), false, "a_1" );
                for( int i = 0; i < 10; ++i ) {
                    BSONObj temp = BSON( "a" << i );
                    theDataFileMgr.insertWithObjMod( ns(), temp );
                }
                BSONObj query = fromjson( "{a:{$in:[2,3,6,9,11]}}" );
                BSONObj order;
                BSONObj hint = fromjson( "{$hint:{a:1}}" );
                auto_ptr< FieldRangeSetPair > frsp( new FieldRangeSetPair( ns(), query ) );
                shared_ptr<QueryPlanSet> s = makeQps( query, order, hint );
                scoped_ptr<QueryPlan> qp( QueryPlan::make( nsd(), 1, s->frsp(), frsp.get(),
                                                           query, order ) );
                boost::shared_ptr<Cursor> c = qp->newCursor();
                double expected[] = { 2, 3, 6, 9 };
                for( int i = 0; i < 4; ++i, c->advance() ) {
                    ASSERT_EQUALS( expected[ i ], c->current().getField( "a" ).number() );
                }
                ASSERT( !c->ok() );

                // now check reverse
                {
                    order = BSON( "a" << -1 );
                    auto_ptr< FieldRangeSetPair > frsp( new FieldRangeSetPair( ns(), query ) );
                    shared_ptr<QueryPlanSet> s = makeQps( query, order, hint );
                    scoped_ptr<QueryPlan> qp( QueryPlan::make( nsd(), 1, s->frsp(), frsp.get(),
                                                               query, order ) );
                    boost::shared_ptr<Cursor> c = qp->newCursor();
                    double expected[] = { 9, 6, 3, 2 };
                    for( int i = 0; i < 4; ++i, c->advance() ) {
                        ASSERT_EQUALS( expected[ i ], c->current().getField( "a" ).number() );
                    }
                    ASSERT( !c->ok() );
                }
            }
        };

        class EqualityThenIn : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << 1 << "b" << 1 ), false, "a_1_b_1" );
                for( int i = 0; i < 10; ++i ) {
                    BSONObj temp = BSON( "a" << 5 << "b" << i );
                    theDataFileMgr.insertWithObjMod( ns(), temp );
                }
                auto_ptr< FieldRangeSetPair > frsp( new FieldRangeSetPair( ns(), fromjson( "{a:5,b:{$in:[2,3,6,9,11]}}" ) ) );
                scoped_ptr<QueryPlan> qp( QueryPlan::make( nsd(), 1, *frsp, frsp.get(),
                                                          fromjson( "{a:5,b:{$in:[2,3,6,9,11]}}" ),
                                                          BSONObj() ) );
                boost::shared_ptr<Cursor> c = qp->newCursor();
                double expected[] = { 2, 3, 6, 9 };
                ASSERT( c->ok() );
                for( int i = 0; i < 4; ++i, c->advance() ) {
                    ASSERT( c->ok() );
                    ASSERT_EQUALS( expected[ i ], c->current().getField( "b" ).number() );
                }
                ASSERT( !c->ok() );
            }
        };

        class NotEqualityThenIn : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << 1 << "b" << 1 ), false, "a_1_b_1" );
                for( int i = 0; i < 10; ++i ) {
                    BSONObj temp = BSON( "a" << 5 << "b" << i );
                    theDataFileMgr.insertWithObjMod( ns(), temp );
                }
                auto_ptr< FieldRangeSetPair > frsp( new FieldRangeSetPair( ns(), fromjson( "{a:{$gte:5},b:{$in:[2,3,6,9,11]}}" ) ) );
                scoped_ptr<QueryPlan> qp
                        ( QueryPlan::make( nsd(), 1, *frsp, frsp.get(),
                                          fromjson( "{a:{$gte:5},b:{$in:[2,3,6,9,11]}}" ),
                                          BSONObj() ) );
                boost::shared_ptr<Cursor> c = qp->newCursor();
                int matches[] = { 2, 3, 6, 9 };
                for( int i = 0; i < 4; ++i, c->advance() ) {
                    ASSERT_EQUALS( matches[ i ], c->current().getField( "b" ).number() );
                }
                ASSERT( !c->ok() );
            }
        };
        
        /** Exclude special plan candidate if there are btree plan candidates. SERVER-4531 */
        class ExcludeSpecialPlanWhenBtreePlan : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << "2d" ), false, "a_2d" );
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), false, "a_1" );
                shared_ptr<QueryPlanSet> s =
                        makeQps( BSON( "a" << BSON_ARRAY( 0 << 0 ) << "b" << 1 ) );
                // Two query plans, btree and collection scan.
                ASSERT_EQUALS( 2, s->nPlans() );
                // Not the geo plan.
                ASSERT( s->firstPlan()->special().empty() );
            }
        };
        
        /** Exclude unindexed plan candidate if there is a special plan candidate. SERVER-4531 */
        class ExcludeUnindexedPlanWhenSpecialPlan : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << "2d" ), false, "a_2d" );
                shared_ptr<QueryPlanSet> s =
                        makeQps( BSON( "a" << BSON_ARRAY( 0 << 0 ) << "b" << 1 ) );
                // Single query plan.
                ASSERT_EQUALS( 1, s->nPlans() );
                // It's the geo plan.
                ASSERT( !s->firstPlan()->special().empty() );                
            }
        };
        
        class PossiblePlans : public Base {
        public:
            void run() {
                client().ensureIndex( ns(), BSON( "a" << 1 ) );
                client().ensureIndex( ns(), BSON( "b" << 1 ) );
                
                {
                    shared_ptr<QueryPlanSet> qps = makeQps( BSON( "a" << 1 ), BSONObj() );
                    ASSERT_EQUALS( 1, qps->nPlans() );
                    ASSERT( qps->possibleInOrderPlan() );
                    ASSERT( qps->haveInOrderPlan() );
                    ASSERT( !qps->possibleOutOfOrderPlan() );
                    ASSERT( !qps->hasPossiblyExcludedPlans() );
                    ASSERT( !qps->usingCachedPlan() );
                }
                
                {
                    shared_ptr<QueryPlanSet> qps = makeQps( BSON( "a" << 1 ), BSON( "b" << 1 ) );
                    ASSERT_EQUALS( 3, qps->nPlans() );
                    ASSERT( qps->possibleInOrderPlan() );
                    ASSERT( qps->haveInOrderPlan() );
                    ASSERT( qps->possibleOutOfOrderPlan() );
                    ASSERT( !qps->hasPossiblyExcludedPlans() );
                    ASSERT( !qps->usingCachedPlan() );
                }
                
                NamespaceDetailsTransient &nsdt = NamespaceDetailsTransient::get( ns() );
                
                nsdt.registerCachedQueryPlanForPattern( makePattern( BSON( "a" << 1 ), BSONObj() ),
                                                       CachedQueryPlan( BSON( "a" << 1 ), 1,
                                                        CandidatePlanCharacter( true, false ) ) );
                {
                    shared_ptr<QueryPlanSet> qps = makeQps( BSON( "a" << 1 ), BSONObj() );
                    ASSERT_EQUALS( 1, qps->nPlans() );
                    ASSERT( qps->possibleInOrderPlan() );
                    ASSERT( qps->haveInOrderPlan() );
                    ASSERT( !qps->possibleOutOfOrderPlan() );
                    ASSERT( !qps->hasPossiblyExcludedPlans() );
                    ASSERT( qps->usingCachedPlan() );
                }

                nsdt.registerCachedQueryPlanForPattern
                        ( makePattern( BSON( "a" << 1 ), BSON( "b" << 1 ) ),
                         CachedQueryPlan( BSON( "a" << 1 ), 1,
                                         CandidatePlanCharacter( true, true ) ) );

                {
                    shared_ptr<QueryPlanSet> qps = makeQps( BSON( "a" << 1 ), BSON( "b" << 1 ) );
                    ASSERT_EQUALS( 1, qps->nPlans() );
                    ASSERT( qps->possibleInOrderPlan() );
                    ASSERT( !qps->haveInOrderPlan() );
                    ASSERT( qps->possibleOutOfOrderPlan() );
                    ASSERT( qps->hasPossiblyExcludedPlans() );
                    ASSERT( qps->usingCachedPlan() );
                }

                nsdt.registerCachedQueryPlanForPattern
                        ( makePattern( BSON( "a" << 1 ), BSON( "b" << 1 ) ),
                         CachedQueryPlan( BSON( "b" << 1 ), 1,
                                         CandidatePlanCharacter( true, true ) ) );
                
                {
                    shared_ptr<QueryPlanSet> qps = makeQps( BSON( "a" << 1 ), BSON( "b" << 1 ) );
                    ASSERT_EQUALS( 1, qps->nPlans() );
                    ASSERT( qps->possibleInOrderPlan() );
                    ASSERT( qps->haveInOrderPlan() );
                    ASSERT( qps->possibleOutOfOrderPlan() );
                    ASSERT( qps->hasPossiblyExcludedPlans() );
                    ASSERT( qps->usingCachedPlan() );
                }
                
                {
                    shared_ptr<QueryPlanSet> qps = makeQps( BSON( "a" << 1 ), BSON( "c" << 1 ) );
                    ASSERT_EQUALS( 2, qps->nPlans() );
                    ASSERT( !qps->possibleInOrderPlan() );
                    ASSERT( !qps->haveInOrderPlan() );
                    ASSERT( qps->possibleOutOfOrderPlan() );
                    ASSERT( !qps->hasPossiblyExcludedPlans() );
                    ASSERT( !qps->usingCachedPlan() );
                }                
            }
        };

        /** An unhelpful query plan will not be used if recorded in the query plan cache. */
        class AvoidUnhelpfulRecordedPlan : public Base {
        public:
            void run() {
                client().ensureIndex( ns(), BSON( "a" << 1 ) );

                // Record the {a:1} index for a {b:1} query.
                NamespaceDetailsTransient &nsdt = NamespaceDetailsTransient::get( ns() );
                nsdt.registerCachedQueryPlanForPattern
                        ( makePattern( BSON( "b" << 1 ), BSONObj() ),
                         CachedQueryPlan( BSON( "a" << 1 ), 1,
                                         CandidatePlanCharacter( true, false ) ) );

                // The {a:1} index is not used for a {b:1} query because it generates an unhelpful
                // plan.
                shared_ptr<QueryPlanSet> qps = makeQps( BSON( "b" << 1 ), BSONObj() );
                ASSERT_EQUALS( 1, qps->nPlans() );
                ASSERT_EQUALS( BSON( "$natural" << 1 ), qps->firstPlan()->indexKey() );
            }
        };
        
        /** An unhelpful query plan will not be used if recorded in the query plan cache. */
        class AvoidDisallowedRecordedPlan : public Base {
        public:
            void run() {
                client().insert( "unittests.system.indexes",
                                BSON( "ns" << ns() <<
                                     "key" << BSON( "a" << 1 ) <<
                                     "name" << client().genIndexName( BSON( "a" <<  1 ) ) <<
                                     "sparse" << true ) );

                // Record the {a:1} index for a {a:null} query.
                NamespaceDetailsTransient &nsdt = NamespaceDetailsTransient::get( ns() );
                nsdt.registerCachedQueryPlanForPattern
                ( makePattern( BSON( "a" << BSONNULL ), BSONObj() ),
                 CachedQueryPlan( BSON( "a" << 1 ), 1,
                                 CandidatePlanCharacter( true, false ) ) );
                
                // The {a:1} index is not used for an {a:{$exists:false}} query because it generates
                // a disallowed plan.
                shared_ptr<QueryPlanSet> qps = makeQps( BSON( "a" << BSON( "$exists" << false ) ),
                                                       BSONObj() );
                ASSERT_EQUALS( 1, qps->nPlans() );
                ASSERT_EQUALS( BSON( "$natural" << 1 ), qps->firstPlan()->indexKey() );
            }
        };

        /** Special plans are only selected when allowed. */
        class AllowSpecial : public Base {
        public:
            void run() {
                BSONObj naturalIndex = BSON( "$natural" << 1 );
                BSONObj specialIndex = BSON( "a" << "2d" );
                BSONObj query = BSON( "a" << BSON_ARRAY( 0 << 0 ) );
                client().ensureIndex( ns(), specialIndex );

                // The special plan is chosen if allowed.
                assertSingleIndex( specialIndex, makeQps( query ) );

                // The special plan is not chosen if not allowed
                assertSingleIndex( naturalIndex, makeQps( query, BSONObj(), BSONObj(), false ) );

                // Attempting to hint a special plan when not allowed triggers an assertion.
                ASSERT_THROWS( makeQps( query, BSONObj(), BSON( "$hint" << specialIndex ), false ),
                               UserException );

                // Attempting to use a geo operator when special plans are not allowed triggers an
                // assertion.
                ASSERT_THROWS( makeQps( BSON( "a" << BSON( "$near" << BSON_ARRAY( 0 << 0 ) ) ),
                                        BSONObj(), BSONObj(), false ),
                               UserException );

                // The special plan is not chosen if not allowed, even if cached.
                NamespaceDetailsTransient &nsdt = NamespaceDetailsTransient::get( ns() );
                nsdt.registerCachedQueryPlanForPattern
                        ( makePattern( query, BSONObj() ),
                          CachedQueryPlan( specialIndex, 1,
                                           CandidatePlanCharacter( true, false ) ) );
                assertSingleIndex( naturalIndex, makeQps( query, BSONObj(), BSONObj(), false ) );
            }
        private:
            void assertSingleIndex( const BSONObj& index, const shared_ptr<QueryPlanSet>& set ) {
                ASSERT_EQUALS( 1, set->nPlans() );
                ASSERT_EQUALS( index, set->firstPlan()->indexKey() );
            }
        };
        
    } // namespace QueryPlanSetTests

    class Base {
    public:
        Base() : _ctx( ns() ) {
            string err;
            userCreateNS( ns(), BSONObj(), err, false );
        }
        ~Base() {
            if ( !nsd() )
                return;
            string s( ns() );
            dropCollection( ns() );
        }
    protected:
        static const char *ns() { return "unittests.QueryOptimizerTests"; }
        static NamespaceDetails *nsd() { return nsdetails( ns() ); }
        QueryPattern makePattern( const BSONObj &query, const BSONObj &order ) {
            FieldRangeSet frs( ns(), query, true, true );
            return QueryPattern( frs, order );
        }
        shared_ptr<MultiPlanScanner> makeMps( const BSONObj &query, const BSONObj &order ) {
            shared_ptr<MultiPlanScanner> ret( MultiPlanScanner::make( ns(), query, order ) );
            return ret;
        }
        DBDirectClient &client() { return _client; }
    private:
        Lock::GlobalWrite lk_;
        Client::Context _ctx;
        DBDirectClient _client;
    };

    namespace MultiPlanScannerTests {
        class ToString : public Base {
        public:
            void run() {
                scoped_ptr<MultiPlanScanner> multiPlanScanner
                        ( MultiPlanScanner::make( ns(), BSON( "a" << 1 ), BSONObj() ) );
                multiPlanScanner->toString(); // Just test that we don't crash.
            }
        };
        
        class PossiblePlans : public Base {
        public:
            void run() {
                client().ensureIndex( ns(), BSON( "a" << 1 ) );
                client().ensureIndex( ns(), BSON( "b" << 1 ) );
                
                {
                    shared_ptr<MultiPlanScanner> mps = makeMps( BSON( "a" << 1 ), BSONObj() );
                    ASSERT_EQUALS( 1, mps->currentNPlans() );
                    ASSERT( mps->possibleInOrderPlan() );
                    ASSERT( mps->haveInOrderPlan() );
                    ASSERT( !mps->possibleOutOfOrderPlan() );
                    ASSERT( !mps->hasPossiblyExcludedPlans() );
                }
                
                {
                    shared_ptr<MultiPlanScanner> mps =
                    makeMps( BSON( "a" << 1 ), BSON( "b" << 1 ) );
                    ASSERT_EQUALS( 3, mps->currentNPlans() );
                    ASSERT( mps->possibleInOrderPlan() );
                    ASSERT( mps->haveInOrderPlan() );
                    ASSERT( mps->possibleOutOfOrderPlan() );
                    ASSERT( !mps->hasPossiblyExcludedPlans() );
                }
                
                NamespaceDetailsTransient &nsdt = NamespaceDetailsTransient::get( ns() );

                nsdt.registerCachedQueryPlanForPattern( makePattern( BSON( "a" << 1 ), BSONObj() ),
                                                       CachedQueryPlan( BSON( "a" << 1 ), 1,
                                                        CandidatePlanCharacter( true, false ) ) );
                {
                    shared_ptr<MultiPlanScanner> mps = makeMps( BSON( "a" << 1 ), BSONObj() );
                    ASSERT_EQUALS( 1, mps->currentNPlans() );
                    ASSERT( mps->possibleInOrderPlan() );
                    ASSERT( mps->haveInOrderPlan() );
                    ASSERT( !mps->possibleOutOfOrderPlan() );
                    ASSERT( !mps->hasPossiblyExcludedPlans() );
                }

                nsdt.registerCachedQueryPlanForPattern
                        ( makePattern( BSON( "a" << 1 ), BSON( "b" << 1 ) ),
                         CachedQueryPlan( BSON( "a" << 1 ), 1,
                                         CandidatePlanCharacter( true, true ) ) );
                
                {
                    shared_ptr<MultiPlanScanner> mps =
                    makeMps( BSON( "a" << 1 ), BSON( "b" << 1 ) );
                    ASSERT_EQUALS( 1, mps->currentNPlans() );
                    ASSERT( mps->possibleInOrderPlan() );
                    ASSERT( !mps->haveInOrderPlan() );
                    ASSERT( mps->possibleOutOfOrderPlan() );
                    ASSERT( mps->hasPossiblyExcludedPlans() );
                }
                
                nsdt.registerCachedQueryPlanForPattern
                        ( makePattern( BSON( "a" << 1 ), BSON( "b" << 1 ) ),
                         CachedQueryPlan( BSON( "b" << 1 ), 1,
                                         CandidatePlanCharacter( true, true ) ) );
                
                {
                    shared_ptr<MultiPlanScanner> mps =
                    makeMps( BSON( "a" << 1 ), BSON( "b" << 1 ) );
                    ASSERT_EQUALS( 1, mps->currentNPlans() );
                    ASSERT( mps->possibleInOrderPlan() );
                    ASSERT( mps->haveInOrderPlan() );
                    ASSERT( mps->possibleOutOfOrderPlan() );
                    ASSERT( mps->hasPossiblyExcludedPlans() );
                }
                
                {
                    shared_ptr<MultiPlanScanner> mps =
                    makeMps( BSON( "a" << 1 ), BSON( "c" << 1 ) );
                    ASSERT_EQUALS( 2, mps->currentNPlans() );
                    ASSERT( !mps->possibleInOrderPlan() );
                    ASSERT( !mps->haveInOrderPlan() );
                    ASSERT( mps->possibleOutOfOrderPlan() );
                    ASSERT( !mps->hasPossiblyExcludedPlans() );
                }
                
                {
                    shared_ptr<MultiPlanScanner> mps =
                    makeMps( fromjson( "{$or:[{a:1},{a:2}]}" ), BSON( "c" << 1 ) );
                    ASSERT_EQUALS( 1, mps->currentNPlans() );
                    ASSERT( !mps->possibleInOrderPlan() );
                    ASSERT( !mps->haveInOrderPlan() );
                    ASSERT( mps->possibleOutOfOrderPlan() );
                    ASSERT( !mps->hasPossiblyExcludedPlans() );
                }

                {
                    shared_ptr<MultiPlanScanner> mps =
                    makeMps( fromjson( "{$or:[{a:1,b:1},{a:2,b:2}]}" ), BSONObj() );
                    ASSERT_EQUALS( 3, mps->currentNPlans() );
                    ASSERT( mps->possibleInOrderPlan() );
                    ASSERT( mps->haveInOrderPlan() );
                    ASSERT( !mps->possibleOutOfOrderPlan() );
                    ASSERT( !mps->hasPossiblyExcludedPlans() );
                }
            }
        };

    } // namespace MultiPlanScannerTests
    
    class BestGuess : public Base {
    public:
        void run() {
            Helpers::ensureIndex( ns(), BSON( "a" << 1 ), false, "a_1" );
            Helpers::ensureIndex( ns(), BSON( "b" << 1 ), false, "b_1" );
            BSONObj temp = BSON( "a" << 1 );
            theDataFileMgr.insertWithObjMod( ns(), temp );
            temp = BSON( "b" << 1 );
            theDataFileMgr.insertWithObjMod( ns(), temp );

            boost::shared_ptr< Cursor > c = getBestGuessCursor( ns(),
                                                                BSON( "b" << 1 ),
                                                                BSON( "a" << 1 ) );
            ASSERT_EQUALS( string( "a" ), c->indexKeyPattern().firstElement().fieldName() );
            
            c = getBestGuessCursor( ns(), BSON( "a" << 1 ), BSON( "b" << 1 ) );
            ASSERT_EQUALS( string( "b" ), c->indexKeyPattern().firstElementFieldName() );
            ASSERT( c->matcher() );
            ASSERT( c->currentMatches() ); // { b:1 } document
            c->advance();
            ASSERT( !c->currentMatches() ); // { a:1 } document
            
            c = getBestGuessCursor( ns(), fromjson( "{b:1,$or:[{z:1}]}" ), BSON( "a" << 1 ) );
            ASSERT_EQUALS( string( "a" ), c->indexKeyPattern().firstElement().fieldName() );

            c = getBestGuessCursor( ns(), fromjson( "{a:1,$or:[{y:1}]}" ), BSON( "b" << 1 ) );
            ASSERT_EQUALS( string( "b" ), c->indexKeyPattern().firstElementFieldName() );

            FieldRangeSet frs( "ns", BSON( "a" << 1 ), true, true );
            {
                SimpleMutex::scoped_lock lk(NamespaceDetailsTransient::_qcMutex);
                NamespaceDetailsTransient::get_inlock( ns() ).
                        registerCachedQueryPlanForPattern( frs.pattern( BSON( "b" << 1 ) ),
                                                          CachedQueryPlan( BSON( "a" << 1 ), 0,
                                                        CandidatePlanCharacter( true, true ) ) );
            }
            
            c = getBestGuessCursor( ns(), fromjson( "{a:1,$or:[{y:1}]}" ), BSON( "b" << 1 ) );
            ASSERT_EQUALS( string( "b" ),
                          c->indexKeyPattern().firstElement().fieldName() );
        }
    };
    
    class All : public Suite {
    public:
        All() : Suite( "queryoptimizer2" ) {}

        void setupTests() {
            add<QueryPlanSetTests::ToString>();
            add<QueryPlanSetTests::NoIndexes>();
            add<QueryPlanSetTests::Optimal>();
            add<QueryPlanSetTests::NoOptimal>();
            add<QueryPlanSetTests::NoSpec>();
            add<QueryPlanSetTests::HintSpec>();
            add<QueryPlanSetTests::HintName>();
            add<QueryPlanSetTests::NaturalHint>();
            add<QueryPlanSetTests::NaturalSort>();
            add<QueryPlanSetTests::BadHint>();
            add<QueryPlanSetTests::Count>();
            add<QueryPlanSetTests::QueryMissingNs>();
            add<QueryPlanSetTests::UnhelpfulIndex>();
            add<QueryPlanSetTests::FindOne>();
            add<QueryPlanSetTests::Delete>();
            add<QueryPlanSetTests::DeleteOneScan>();
            add<QueryPlanSetTests::DeleteOneIndex>();
            add<QueryPlanSetTests::InQueryIntervals>();
            add<QueryPlanSetTests::EqualityThenIn>();
            add<QueryPlanSetTests::NotEqualityThenIn>();
            add<QueryPlanSetTests::ExcludeSpecialPlanWhenBtreePlan>();
            add<QueryPlanSetTests::ExcludeUnindexedPlanWhenSpecialPlan>();
            add<QueryPlanSetTests::PossiblePlans>();
            add<QueryPlanSetTests::AvoidUnhelpfulRecordedPlan>();
            add<QueryPlanSetTests::AvoidDisallowedRecordedPlan>();
            add<QueryPlanSetTests::AllowSpecial>();
            add<MultiPlanScannerTests::ToString>();
            add<MultiPlanScannerTests::PossiblePlans>();
            add<BestGuess>();
        }
    } myall;

} // namespace QueryOptimizerTests

