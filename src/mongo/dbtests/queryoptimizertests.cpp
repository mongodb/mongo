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

#include "mongo/db/instance.h"
#include "mongo/db/json.h"
#include "mongo/db/ops/count.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/ops/query.h"
#include "mongo/db/parsed_query.h"
#include "mongo/db/query_optimizer_internal.h"
#include "mongo/db/queryutil.h"
#include "mongo/dbtests/dbtests.h"


namespace mongo {
    extern BSONObj id_obj;
    void runQuery(Message& m, QueryMessage& q, Message &response ) {
        CurOp op( &(cc()) );
        op.ensureStarted();
        runQuery( m , q , op, response );
    }
    void runQuery(Message& m, QueryMessage& q ) {
        Message response;
        runQuery( m, q, response );
    }
    void __forceLinkGeoPlugin();
} // namespace mongo

namespace {

    using boost::shared_ptr;
    
    void dropCollection( const char *ns ) {
     	string errmsg;
        BSONObjBuilder result;
        dropCollection( ns, errmsg, result );
    }
    
    namespace QueryPlanTests {

        class ToString {
        public:
            void run() {
                BSONObj obj = BSON( "a" << 1 );
                FieldRangeSetPair fieldRangeSetPair( "", obj );
                BSONObj order = BSON( "b" << 1 );
                scoped_ptr<QueryPlan> queryPlan( QueryPlan::make( 0, -1, fieldRangeSetPair, 0, obj,
                                                                 order ) );
                queryPlan->toString(); // Just test that we don't crash.
            }
        };

        class Base {
        public:
            Base() : _ctx( ns() ) , indexNum_( 0 ) {
                string err;
                userCreateNS( ns(), BSONObj(), err, false );
            }
            ~Base() {
                if ( !nsd() )
                    return;
                dropCollection( ns() );
            }
        protected:
            static const char *ns() { return "unittests.QueryPlanTests"; }
            static NamespaceDetails *nsd() { return nsdetails( ns() ); }
            IndexDetails *index( const BSONObj &key ) {
                stringstream ss;
                ss << indexNum_++;
                string name = ss.str();
                client_.resetIndexCache();
                client_.ensureIndex( ns(), key, false, name.c_str() );
                return &nsd()->idx( existingIndexNo( key ) );
            }
            int indexno( const BSONObj &key ) {
                return nsd()->idxNo( *index(key) );
            }
            int existingIndexNo( const BSONObj &key ) const {
                NamespaceDetails *d = nsd();
                for( int i = 0; i < d->getCompletedIndexCount(); ++i ) {
                    if ( ( d->idx( i ).keyPattern() == key ) ||
                        ( d->idx( i ).isIdIndex() && IndexDetails::isIdIndexPattern( key ) ) ) {
                        return i;
                    }
                }
                verify( false );
                return -1;
            }
            BSONObj startKey( const QueryPlan &p ) const {
                return p.frv()->startKey();
            }
            BSONObj endKey( const QueryPlan &p ) const {
                return p.frv()->endKey();
            }
            DBDirectClient &client() const { return client_; }

        private:
            Lock::GlobalWrite lk_;
            Client::Context _ctx;
            int indexNum_;
            static DBDirectClient client_;
        };
        DBDirectClient Base::client_;

        // There's a limit of 10 indexes total, make sure not to exceed this in a given test.
#define INDEXNO(x) nsd()->idxNo( *this->index( BSON(x) ) )
#define INDEX(x) this->index( BSON(x) )
        auto_ptr< FieldRangeSetPair > FieldRangeSetPair_GLOBAL;
#define FRSP(x) ( FieldRangeSetPair_GLOBAL.reset( new FieldRangeSetPair( ns(), x ) ), *FieldRangeSetPair_GLOBAL )
        auto_ptr< FieldRangeSetPair > FieldRangeSetPair_GLOBAL2;
#define FRSP2(x) ( FieldRangeSetPair_GLOBAL2.reset( new FieldRangeSetPair( ns(), x ) ), FieldRangeSetPair_GLOBAL2.get() )

        class NoIndex : public Base {
        public:
            void run() {
                scoped_ptr<QueryPlan> p( QueryPlan::make( nsd(), -1, FRSP( BSONObj() ), 
                                                         FRSP2( BSONObj() ), BSONObj(),
                                                         BSONObj() ) );
                ASSERT_EQUALS( QueryPlan::Helpful, p->utility() );
                ASSERT( !p->scanAndOrderRequired() );
                ASSERT( p->mayBeMatcherNecessary() );
            }
        };

        class SimpleOrder : public Base {
        public:
            void run() {
                BSONObjBuilder b;
                b.appendMinKey( "" );
                BSONObj start = b.obj();
                BSONObjBuilder b2;
                b2.appendMaxKey( "" );
                BSONObj end = b2.obj();

                scoped_ptr<QueryPlan> p( QueryPlan::make( nsd(), INDEXNO( "a" << 1 ),
                                                         FRSP( BSONObj() ), FRSP2( BSONObj() ),
                                                         BSONObj(), BSON( "a" << 1 ) ) );
                ASSERT( !p->scanAndOrderRequired() );
                ASSERT( !startKey( *p ).woCompare( start ) );
                ASSERT( !endKey( *p ).woCompare( end ) );
                scoped_ptr<QueryPlan> p2( QueryPlan::make( nsd(), INDEXNO( "a" << 1 << "b" << 1 ),
                                                          FRSP( BSONObj() ), FRSP2( BSONObj() ),
                                                          BSONObj(),
                                                          BSON( "a" << 1 << "b" << 1 ) ) );
                ASSERT( !p2->scanAndOrderRequired() );
                scoped_ptr<QueryPlan> p3( QueryPlan::make( nsd(), INDEXNO( "a" << 1 ),
                                                          FRSP( BSONObj() ), FRSP2( BSONObj() ),
                                                          BSONObj(), BSON( "b" << 1 ) ) );
                ASSERT( p3->scanAndOrderRequired() );
                ASSERT( !startKey( *p3 ).woCompare( start ) );
                ASSERT( !endKey( *p3 ).woCompare( end ) );
            }
        };

        class MoreIndexThanNeeded : public Base {
        public:
            void run() {
                scoped_ptr<QueryPlan> p( QueryPlan::make( nsd(), INDEXNO( "a" << 1 << "b" << 1 ),
                                                         FRSP( BSONObj() ), FRSP2( BSONObj() ),
                                                         BSONObj(), BSON( "a" << 1 ) ) );
                ASSERT( !p->scanAndOrderRequired() );
            }
        };

        class IndexSigns : public Base {
        public:
            void run() {
                scoped_ptr<QueryPlan> p( QueryPlan::make( nsd(), INDEXNO( "a" << 1 << "b" << -1 ),
                                                         FRSP( BSONObj() ), FRSP2( BSONObj() ),
                                                         BSONObj(),
                                                         BSON( "a" << 1 << "b" << -1 ) ) );
                ASSERT( !p->scanAndOrderRequired() );
                ASSERT_EQUALS( 1, p->direction() );
                scoped_ptr<QueryPlan> p2( QueryPlan::make( nsd(), INDEXNO( "a" << 1 << "b" << 1 ),
                                                          FRSP( BSONObj() ), FRSP2( BSONObj() ),
                                                          BSONObj(),
                                                          BSON( "a" << 1 << "b" << -1 ) ) );
                ASSERT( p2->scanAndOrderRequired() );
                ASSERT_EQUALS( 0, p2->direction() );
                scoped_ptr<QueryPlan> p3( QueryPlan::make( nsd(), indexno( id_obj ),
                                                          FRSP( BSONObj() ), FRSP2( BSONObj() ),
                                                          BSONObj(), BSON( "_id" << 1 ) ) );
                ASSERT( !p3->scanAndOrderRequired() );
                ASSERT_EQUALS( 1, p3->direction() );
            }
        };

        class IndexReverse : public Base {
        public:
            void run() {
                BSONObjBuilder b;
                b.appendMinKey( "" );
                b.appendMaxKey( "" );
                BSONObj start = b.obj();
                BSONObjBuilder b2;
                b2.appendMaxKey( "" );
                b2.appendMinKey( "" );
                BSONObj end = b2.obj();
                scoped_ptr<QueryPlan> p( QueryPlan::make( nsd(), INDEXNO( "a" << -1 << "b" << 1 ),
                                                         FRSP( BSONObj() ), FRSP2( BSONObj() ),
                                                         BSONObj(),
                                                         BSON( "a" << 1 << "b" << -1 ) ) );
                ASSERT( !p->scanAndOrderRequired() );
                ASSERT_EQUALS( -1, p->direction() );
                ASSERT( !startKey( *p ).woCompare( start ) );
                ASSERT( !endKey( *p ).woCompare( end ) );
                scoped_ptr<QueryPlan> p2( QueryPlan::make( nsd(), INDEXNO( "a" << 1 << "b" << 1 ),
                                                          FRSP( BSONObj() ), FRSP2( BSONObj() ),
                                                          BSONObj(),
                                                          BSON( "a" << -1 << "b" << -1 ) ) );
                ASSERT( !p2->scanAndOrderRequired() );
                ASSERT_EQUALS( -1, p2->direction() );
                scoped_ptr<QueryPlan> p3( QueryPlan::make( nsd(), INDEXNO( "a" << 1 << "b" << -1 ),
                                                          FRSP( BSONObj() ), FRSP2( BSONObj() ),
                                                          BSONObj(),
                                                          BSON( "a" << -1 << "b" << -1 ) ) );
                ASSERT( p3->scanAndOrderRequired() );
                ASSERT_EQUALS( 0, p3->direction() );
            }
        };

        class NoOrder : public Base {
        public:
            void run() {
                BSONObjBuilder b;
                b.append( "", 3 );
                b.appendMinKey( "" );
                BSONObj start = b.obj();
                BSONObjBuilder b2;
                b2.append( "", 3 );
                b2.appendMaxKey( "" );
                BSONObj end = b2.obj();
                scoped_ptr<QueryPlan> p( QueryPlan::make( nsd(), INDEXNO( "a" << -1 << "b" << 1 ),
                                                         FRSP( BSON( "a" << 3 ) ),
                                                         FRSP2( BSON( "a" << 3 ) ),
                                                         BSON( "a" << 3 ), BSONObj() ) );
                ASSERT( !p->scanAndOrderRequired() );
                ASSERT( !startKey( *p ).woCompare( start ) );
                ASSERT( !endKey( *p ).woCompare( end ) );
                scoped_ptr<QueryPlan> p2( QueryPlan::make( nsd(), INDEXNO( "a" << -1 << "b" << 1 ),
                                                          FRSP( BSON( "a" << 3 ) ),
                                                          FRSP2( BSON( "a" << 3 ) ),
                                                          BSON( "a" << 3 ), BSONObj() ) );
                ASSERT( !p2->scanAndOrderRequired() );
                ASSERT( !startKey( *p ).woCompare( start ) );
                ASSERT( !endKey( *p ).woCompare( end ) );
            }
        };

        class EqualWithOrder : public Base {
        public:
            void run() {
                scoped_ptr<QueryPlan> p( QueryPlan::make( nsd(), INDEXNO( "a" << 1 << "b" << 1 ),
                                                         FRSP( BSON( "a" << 4 ) ),
                                                         FRSP2( BSON( "a" << 4 ) ),
                                                         BSON( "a" << 4 ), BSON( "b" << 1 ) ) );
                ASSERT( !p->scanAndOrderRequired() );
                scoped_ptr<QueryPlan> p2
                        ( QueryPlan::make( nsd(), INDEXNO( "a" << 1 << "b" << 1 << "c" << 1 ),
                                          FRSP( BSON( "b" << 4 ) ), FRSP2( BSON( "b" << 4 ) ),
                                          BSON( "b" << 4 ), BSON( "a" << 1 << "c" << 1 ) ) );
                ASSERT( !p2->scanAndOrderRequired() );
                scoped_ptr<QueryPlan> p3( QueryPlan::make( nsd(), INDEXNO( "a" << 1 << "b" << 1 ),
                                                          FRSP( BSON( "b" << 4 ) ),
                                                          FRSP2( BSON( "b" << 4 ) ),
                                                          BSON( "b" << 4 ),
                                                          BSON( "a" << 1 << "c" << 1 ) ) );
                ASSERT( p3->scanAndOrderRequired() );
            }
        };

        class Optimal : public Base {
        public:
            void run() {
                scoped_ptr<QueryPlan> p( QueryPlan::make( nsd(), INDEXNO( "a" << 1 ),
                                                         FRSP( BSONObj() ), FRSP2( BSONObj() ),
                                                         BSONObj(), BSON( "a" << 1 ) ) );
                ASSERT_EQUALS( QueryPlan::Optimal, p->utility() );
                scoped_ptr<QueryPlan> p2( QueryPlan::make( nsd(), INDEXNO( "a" << 1 << "b" << 1 ),
                                                          FRSP( BSONObj() ), FRSP2( BSONObj() ),
                                                          BSONObj(), BSON( "a" << 1 ) ) );
                ASSERT_EQUALS( QueryPlan::Optimal, p2->utility() );
                scoped_ptr<QueryPlan> p3( QueryPlan::make( nsd(), INDEXNO( "a" << 1 << "b" << 1 ),
                                                          FRSP( BSON( "a" << 1 ) ),
                                                          FRSP2( BSON( "a" << 1 ) ),
                                                          BSON( "a" << 1 ), BSON( "a" << 1 ) ) );
                ASSERT_EQUALS( QueryPlan::Optimal, p3->utility() );
                scoped_ptr<QueryPlan> p4( QueryPlan::make( nsd(), INDEXNO( "a" << 1 << "b" << 1 ),
                                                          FRSP( BSON( "b" << 1 ) ),
                                                          FRSP2( BSON( "b" << 1 ) ),
                                                          BSON( "b" << 1 ), BSON( "a" << 1 ) ) );
                ASSERT_EQUALS( QueryPlan::Helpful, p4->utility() );
                scoped_ptr<QueryPlan> p5( QueryPlan::make( nsd(), INDEXNO( "a" << 1 << "b" << 1 ),
                                                          FRSP( BSON( "a" << 1 ) ),
                                                          FRSP2( BSON( "a" << 1 ) ),
                                                          BSON( "a" << 1 ), BSON( "b" << 1 ) ) );
                ASSERT_EQUALS( QueryPlan::Optimal, p5->utility() );
                scoped_ptr<QueryPlan> p6( QueryPlan::make( nsd(), INDEXNO( "a" << 1 << "b" << 1 ),
                                                          FRSP( BSON( "b" << 1 ) ),
                                                          FRSP2( BSON( "b" << 1 ) ),
                                                          BSON( "b" << 1 ), BSON( "b" << 1 ) ) );
                ASSERT_EQUALS( QueryPlan::Unhelpful, p6->utility() );
                scoped_ptr<QueryPlan> p7( QueryPlan::make( nsd(), INDEXNO( "a" << 1 << "b" << 1 ),
                                                          FRSP( BSON( "a" << 1 << "b" << 1 ) ),
                                                          FRSP2( BSON( "a" << 1 << "b" << 1 ) ),
                                                          BSON( "a" << 1 << "b" << 1 ),
                                                          BSON( "a" << 1 ) ) );
                ASSERT_EQUALS( QueryPlan::Optimal, p7->utility() );
                scoped_ptr<QueryPlan> p8
                        ( QueryPlan::make( nsd(), INDEXNO( "a" << 1 << "b" << 1 ),
                                          FRSP( BSON( "a" << 1 << "b" << LT << 1 ) ),
                                          FRSP2( BSON( "a" << 1 << "b" << LT << 1 ) ),
                                          BSON( "a" << 1 << "b" << LT << 1 ),
                                          BSON( "a" << 1 )  ) );
                ASSERT_EQUALS( QueryPlan::Optimal, p8->utility() );
                scoped_ptr<QueryPlan> p9( QueryPlan::make
                                         ( nsd(), INDEXNO( "a" << 1 << "b" << 1 << "c" << 1 ),
                                          FRSP( BSON( "a" << 1 << "b" << LT << 1 ) ),
                                          FRSP2( BSON( "a" << 1 << "b" << LT << 1 ) ),
                                          BSON( "a" << 1 << "b" << LT << 1 ),
                                          BSON( "a" << 1 ) ) );
                ASSERT_EQUALS( QueryPlan::Optimal, p9->utility() );
            }
        };

        class MoreOptimal : public Base {
        public:
            void run() {
                scoped_ptr<QueryPlan> p10
                        ( QueryPlan::make( nsd(), INDEXNO( "a" << 1 << "b" << 1 << "c" << 1 ),
                                          FRSP( BSON( "a" << 1 ) ), FRSP2( BSON( "a" << 1 ) ),
                                          BSON( "a" << 1 ), BSONObj() ) );
                ASSERT_EQUALS( QueryPlan::Optimal, p10->utility() );
                scoped_ptr<QueryPlan> p11
                        ( QueryPlan::make( nsd(), INDEXNO( "a" << 1 << "b" << 1 << "c" << 1 ),
                                          FRSP( BSON( "a" << 1 << "b" << LT << 1 ) ),
                                          FRSP2( BSON( "a" << 1 << "b" << LT << 1 ) ),
                                          BSON( "a" << 1 << "b" << LT << 1 ),
                                          BSONObj() ) );
                ASSERT_EQUALS( QueryPlan::Optimal, p11->utility() );
                scoped_ptr<QueryPlan> p12
                        ( QueryPlan::make( nsd(), INDEXNO( "a" << 1 << "b" << 1 << "c" << 1 ),
                                          FRSP( BSON( "a" << LT << 1 ) ),
                                          FRSP2( BSON( "a" << LT << 1 ) ), BSON( "a" << LT << 1 ),
                                          BSONObj() ) );
                ASSERT_EQUALS( QueryPlan::Optimal, p12->utility() );
                scoped_ptr<QueryPlan> p13
                        ( QueryPlan::make( nsd(), INDEXNO( "a" << 1 << "b" << 1 << "c" << 1 ),
                                          FRSP( BSON( "a" << LT << 1 ) ),
                                          FRSP2( BSON( "a" << LT << 1 ) ),
                                          BSON( "a" << LT << 1 ), BSON( "a" << 1 ) ) );
                ASSERT_EQUALS( QueryPlan::Optimal, p13->utility() );
            }
        };
        
        /** Cases where a QueryPlan's Utility is Impossible. */
        class Impossible : public Base {
        public:
            void run() {
                // When no match is possible on an indexed field, the plan is Impossible.
                BSONObj impossibleQuery = BSON( "a" << BSON( "$in" << BSONArray() ) );
                scoped_ptr<QueryPlan> p1( QueryPlan::make( nsd(), INDEXNO( "a" << 1 ),
                                                          FRSP( impossibleQuery ),
                                                          FRSP2( impossibleQuery ), impossibleQuery,
                                                          BSONObj() ) );
                ASSERT_EQUALS( QueryPlan::Impossible, p1->utility() );
                // When no match is possible on an unindexed field, the plan is Helpful.
                // (Descriptive test only.)
                BSONObj bImpossibleQuery = BSON( "a" << 1 << "b" << BSON( "$in" << BSONArray() ) );
                scoped_ptr<QueryPlan> p2( QueryPlan::make( nsd(), INDEXNO( "a" << 1 ),
                                                          FRSP( bImpossibleQuery ),
                                                          FRSP2( bImpossibleQuery ),
                                                          bImpossibleQuery, BSONObj() ) );
                ASSERT_EQUALS( QueryPlan::Helpful, p2->utility() );
            }
        };

        /**
         * QueryPlan::mayBeMatcherNecessary() returns false when an index is optimal and a field
         * range set mustBeExactMatchRepresentation() (for a single key index).
         */
        class NotMatcherNecessary : public Base {
        public:
            void run() {
                // Non compound index tests.
                ASSERT( !matcherNecessary( BSON( "a" << 1 ), BSON( "a" << 5 ) ) );
                ASSERT( !matcherNecessary( BSON( "a" << 1 ), BSON( "a" << GT << 5 ) ) );
                ASSERT( !matcherNecessary( BSON( "a" << 1 ), BSON( "a" << GT << 5 << LT << 10 ) ) );
                ASSERT( !matcherNecessary( BSON( "a" << 1 ),
                                           BSON( "a" << BSON( "$in" << BSON_ARRAY( 1 << 2 ) ) ) ) );
                // Compound index tests.
                ASSERT( !matcherNecessary( BSON( "a" << 1 << "b" << 1 ),
                                           BSON( "a" << 5 << "b" << 6 ) ) );
                ASSERT( !matcherNecessary( BSON( "a" << 1 << "b" << -1 ),
                                           BSON( "a" << 2 << "b" << GT << 5 ) ) );
                ASSERT( !matcherNecessary( BSON( "a" << -1 << "b" << 1 ),
                                           BSON( "a" << 3 << "b" << GT << 5 << LT << 10 ) ) );
                ASSERT( !matcherNecessary( BSON( "a" << -1 << "b" << -1 ),
                                           BSON( "a" << "q" <<
                                                 "b" << BSON( "$in" << BSON_ARRAY( 1 << 2 ) ) ) ) );
            }
        private:
            bool matcherNecessary( const BSONObj& index, const BSONObj& query ) {
                scoped_ptr<QueryPlan> plan( makePlan( index, query ) );
                return plan->mayBeMatcherNecessary();
            }
            QueryPlan* makePlan( const BSONObj& index, const BSONObj& query ) {
                return QueryPlan::make( nsd(),
                                        nsd()->idxNo( *this->index( index ) ),
                                        FRSP( query ),
                                        FRSP2( query ),
                                        query,
                                        BSONObj() );
            }
        };

        /**
         * QueryPlan::mayBeMatcherNecessary() returns true when an index is not optimal or a field
         * range set !mustBeExactMatchRepresentation().
         */
        class MatcherNecessary : public Base {
        public:
            void run() {
                // Not mustBeExactMatchRepresentation.
                ASSERT( matcherNecessary( BSON( "a" << 1 ), BSON( "a" << BSON_ARRAY( 5 ) ) ) );
                ASSERT( matcherNecessary( BSON( "a" << 1 ), BSON( "a" << NE << 5 ) ) );
                ASSERT( matcherNecessary( BSON( "a" << 1 ), fromjson( "{a:/b/}" ) ) );
                ASSERT( matcherNecessary( BSON( "a" << 1 ),
                                          BSON( "a" << 1 << "$where" << "false" ) ) );
                // Not optimal index.
                ASSERT( matcherNecessary( BSON( "a" << 1 ), BSON( "a" << 5 << "b" << 6 ) ) );
                ASSERT( matcherNecessary( BSON( "a" << 1 << "b" << -1 ), BSON( "b" << GT << 5 ) ) );
                ASSERT( matcherNecessary( BSON( "a" << -1 << "b" << 1 ),
                                          BSON( "a" << GT << 2 << "b" << LT << 10 ) ) );
                ASSERT( matcherNecessary( BSON( "a" << -1 << "b" << -1 ),
                                          BSON( "a" << BSON( "$in" << BSON_ARRAY( 1 << 2 ) ) <<
                                                "b" << "q" ) ) );
                // Not mustBeExactMatchRepresentation and not optimal index.
                ASSERT( matcherNecessary( BSON( "a" << 1 << "b" << 1 ),
                                          BSON( "b" << BSON_ARRAY( 5 ) ) ) );
            }
        private:
            bool matcherNecessary( const BSONObj& index, const BSONObj& query ) {
                scoped_ptr<QueryPlan> plan( makePlan( index, query ) );
                return plan->mayBeMatcherNecessary();
            }
            QueryPlan* makePlan( const BSONObj& index, const BSONObj& query ) {
                return QueryPlan::make( nsd(),
                                        nsd()->idxNo( *this->index( index ) ),
                                        FRSP( query ),
                                        FRSP2( query ),
                                        query,
                                        BSONObj() );
            }
        };

        /**
         * QueryPlan::mustBeMatcherNecessary() returns true when field ranges on a multikey index
         * cannot be intersected for a single field or across multiple fields.
         */
        class MatcherNecessaryMultikey : public Base {
        public:
            MatcherNecessaryMultikey() {
                client().insert( ns(), fromjson( "{ a:[ { b:1, c:1 }, { b:2, c:2 } ] }" ) );
            }
            void run() {
                ASSERT( !matcherNecessary( BSON( "a" << 1 ), BSON( "a" << GT << 4 ) ) );
                ASSERT( !matcherNecessary( BSON( "a" << 1 << "b" << 1 ),
                                           BSON( "a" << 4 << "b" << LT << 8 ) ) );
                // The two constraints on 'a' cannot be intersected for a multikey index on 'a'.
                ASSERT( matcherNecessary( BSON( "a" << 1 ), BSON( "a" << GT << 4 << LT << 8 ) ) );
                ASSERT( !matcherNecessary( BSON( "a.b" << 1 ), BSON( "a.b" << 5 ) ) );
                ASSERT( !matcherNecessary( BSON( "a.b" << 1 << "c.d" << 1 ),
                                           BSON( "a.b" << 5 << "c.d" << 6 ) ) );
                // The constraints on 'a.b' and 'a.c' cannot be intersected, see comments on
                // SERVER-958 in FieldRangeVector().
                ASSERT( matcherNecessary( BSON( "a.b" << 1 << "a.c" << 1 ),
                                          BSON( "a.b" << 5 << "a.c" << 6 ) ) );
                // The constraints on 'a.b' and 'a.c' can be intersected, but
                // mustBeExactMatchRepresentation() is false for an '$elemMatch' query.
                ASSERT( matcherNecessary( BSON( "a.b" << 1 << "a.c" << 1 ),
                                          fromjson( "{ a:{ $elemMatch:{ b:5, c:6 } } }" ) ) );
            }
        private:
            bool matcherNecessary( const BSONObj& index, const BSONObj& query ) {
                scoped_ptr<QueryPlan> plan( makePlan( index, query ) );
                return plan->mayBeMatcherNecessary();
            }
            QueryPlan* makePlan( const BSONObj& index, const BSONObj& query ) {
                return QueryPlan::make( nsd(),
                                        nsd()->idxNo( *this->index( index ) ),
                                        FRSP( query ),
                                        FRSP2( query ),
                                        query,
                                        BSONObj() );
            }
        };

        class Unhelpful : public Base {
        public:
            void run() {
                scoped_ptr<QueryPlan> p( QueryPlan::make( nsd(), INDEXNO( "a" << 1 << "b" << 1 ),
                                                         FRSP( BSON( "b" << 1 ) ),
                                                         FRSP2( BSON( "b" << 1 ) ),
                                                         BSON( "b" << 1 ), BSONObj() ) );
                ASSERT( p->multikeyFrs().range( "a" ).universal() );
                ASSERT_EQUALS( QueryPlan::Unhelpful, p->utility() );
                scoped_ptr<QueryPlan> p2( QueryPlan::make( nsd(), INDEXNO( "a" << 1 << "b" << 1 ),
                                                          FRSP( BSON( "b" << 1 << "c" << 1 ) ),
                                                          FRSP2( BSON( "b" << 1 << "c" << 1 ) ),
                                                          BSON( "b" << 1 << "c" << 1 ),
                                                          BSON( "a" << 1 ) ) );
                ASSERT( !p2->scanAndOrderRequired() );
                ASSERT( p2->multikeyFrs().range( "a" ).universal() );
                ASSERT_EQUALS( QueryPlan::Helpful, p2->utility() );
                scoped_ptr<QueryPlan> p3( QueryPlan::make( nsd(), INDEXNO( "b" << 1 ),
                                                          FRSP( BSON( "b" << 1 << "c" << 1 ) ),
                                                          FRSP2( BSON( "b" << 1 << "c" << 1 ) ),
                                                          BSON( "b" << 1 << "c" << 1 ),
                                                          BSONObj() ) );
                ASSERT( !p3->multikeyFrs().range( "b" ).universal() );
                ASSERT_EQUALS( QueryPlan::Helpful, p3->utility() );
                scoped_ptr<QueryPlan> p4( QueryPlan::make( nsd(), INDEXNO( "b" << 1 << "c" << 1 ),
                                                          FRSP( BSON( "c" << 1 << "d" << 1 ) ),
                                                          FRSP2( BSON( "c" << 1 << "d" << 1 ) ),
                                                          BSON( "c" << 1 << "d" << 1 ),
                                                          BSONObj() ) );
                ASSERT( p4->multikeyFrs().range( "b" ).universal() );
                ASSERT_EQUALS( QueryPlan::Unhelpful, p4->utility() );
            }
        };
        
        class KeyFieldsOnly : public Base {
        public:
            void run() {
                int idx = INDEXNO( "a" << 1 );

                // No fields supplied.
                scoped_ptr<QueryPlan> p( QueryPlan::make( nsd(), idx, FRSP( BSON( "a" << 1 ) ),
                                                         FRSP2( BSON( "a" << 1 ) ),
                                                         BSON( "a" << 1 ), BSONObj() ) );
                ASSERT( !p->keyFieldsOnly() );
                
                // Fields supplied.
                shared_ptr<ParsedQuery> parsedQuery
                        ( new ParsedQuery( ns(), 0, 0, 0, BSONObj(),
                                          BSON( "_id" << 0 << "a" << 1 ) ) );
                scoped_ptr<QueryPlan> p2( QueryPlan::make( nsd(), idx, FRSP( BSON( "a" << 1 ) ),
                                                          FRSP2( BSON( "a" << 1 ) ),
                                                          BSON( "a" << 1 ), BSONObj(),
                                                          parsedQuery ) );
                ASSERT( p2->keyFieldsOnly() );
                ASSERT_EQUALS( BSON( "a" << 4 ), p2->keyFieldsOnly()->hydrate( BSON( "" << 4 ) ) );
                
                // Fields supplied, but index is multikey.
                DBDirectClient client;
                client.insert( ns(), BSON( "a" << BSON_ARRAY( 1 << 2 ) ) );
                scoped_ptr<QueryPlan> p3( QueryPlan::make( nsd(), idx, FRSP( BSON( "a" << 1 ) ),
                                                          FRSP2( BSON( "a" << 1 ) ),
                                                          BSON( "a" << 1 ), BSONObj(),
                                                          parsedQuery ) );
                ASSERT( !p3->keyFieldsOnly() );
            }
        };
        
        /** $exists:false and some $exists:true predicates disallow sparse index query plans. */
        class SparseExistsFalse : public Base {
        public:
            void run() {
                client().insert( "unittests.system.indexes",
                                BSON( "ns" << ns() <<
                                     "key" << BSON( "a" << 1 ) <<
                                     "name" << client().genIndexName( BSON( "a" <<  1 ) ) <<
                                     "sparse" << true ) );

                // Non $exists predicates allow the sparse index.
                assertAllowed( BSON( "a" << 1 ) );
                assertAllowed( BSON( "b" << 1 ) );

                // Top level $exists:false and $not:{$exists:true} queries disallow the sparse
                // index, regardless of query field.  Otherwise the sparse index is allowed.
                assertDisallowed( BSON( "a" << BSON( "$exists" << false ) ) );
                assertDisallowed( BSON( "b" << BSON( "$exists" << false ) ) );
                assertAllowed( BSON( "a" << BSON( "$exists" << true ) ) );
                assertAllowed( BSON( "b" << BSON( "$exists" << true ) ) );
                assertAllowed( BSON( "a" << BSON( "$not" << BSON( "$exists" << false ) ) ) );
                assertAllowed( BSON( "b" << BSON( "$not" << BSON( "$exists" << false ) ) ) );
                assertDisallowed( BSON( "a" << BSON( "$not" << BSON( "$exists" << true ) ) ) );
                assertDisallowed( BSON( "b" << BSON( "$not" << BSON( "$exists" << true ) ) ) );

                // All nested non $exists predicates allow the sparse index.
                assertAllowed( BSON( "$nor" << BSON_ARRAY( BSON( "a" << 1 ) ) ) );
                assertAllowed( BSON( "$nor" << BSON_ARRAY( BSON( "b" << 1 ) ) ) );

                // All nested $exists predicates disallow the sparse index.
                assertDisallowed( BSON( "$nor" << BSON_ARRAY
                                       ( BSON( "a" << BSON( "$exists" << false ) ) ) ) );
                assertDisallowed( BSON( "$nor" << BSON_ARRAY
                                       ( BSON( "b" << BSON( "$exists" << false ) ) ) ) );
                assertDisallowed( BSON( "$nor" << BSON_ARRAY
                                       ( BSON( "a" << BSON( "$exists" << true ) ) ) ) );
                assertDisallowed( BSON( "$nor" << BSON_ARRAY
                                       ( BSON( "b" << BSON( "$exists" << true ) ) ) ) );
                assertDisallowed( BSON( "$nor" << BSON_ARRAY
                                       ( BSON( "a" <<
                                              BSON( "$not" << BSON( "$exists" << false ) ) ) ) ) );
                assertDisallowed( BSON( "$nor" << BSON_ARRAY
                                       ( BSON( "b" <<
                                              BSON( "$not" << BSON( "$exists" << false ) ) ) ) ) );
                assertDisallowed( BSON( "$nor" << BSON_ARRAY
                                       ( BSON( "a" <<
                                              BSON( "$not" << BSON( "$exists" << true ) ) ) ) ) );
                assertDisallowed( BSON( "$nor" << BSON_ARRAY
                                       ( BSON( "b" <<
                                              BSON( "$not" << BSON( "$exists" << true ) ) ) ) ) );
            }
        private:
            shared_ptr<QueryPlan> newPlan( const BSONObj &query ) const {
                shared_ptr<QueryPlan> ret
                        ( QueryPlan::make( nsd(), existingIndexNo( BSON( "a" << 1 ) ),
                                           FRSP( query ), FRSP2( query ), query, BSONObj() ) );
                return ret;
            }
            void assertAllowed( const BSONObj &query ) const {
                ASSERT_NOT_EQUALS( QueryPlan::Disallowed, newPlan( query )->utility() );
            }
            void assertDisallowed( const BSONObj &query ) const {
                ASSERT_EQUALS( QueryPlan::Disallowed, newPlan( query )->utility() );
            }
        };

        /** Test conditions in which QueryPlan::newCursor() returns an IntervalBtreeCursor. */
        class IntervalCursorCreate : public Base {
        public:
            void run() {
                // An interval cursor will not be created if the query plan is not Optimal.  (See
                // comments on Optimal value of QueryPlan::Utility enum.)
                BSONObj query = fromjson( "{a:{$gt:4},b:{$gt:5}}" );
                scoped_ptr<QueryPlan> plan( QueryPlan::make( nsd(),
                                                             INDEXNO( "a" << 1 << "b" << 1 ),
                                                             FRSP( query ),
                                                             FRSP2( query ),
                                                             query,
                                                             BSONObj() ) );
                shared_ptr<Cursor> cursor = plan->newCursor( DiskLoc(), true );
                ASSERT_NOT_EQUALS( "IntervalBtreeCursor", cursor->toString() );

                // An interval cursor will not be created if the query plan is Optimal but does not
                // consist of a single interval.
                query = fromjson( "{a:4,b:{$in:[5,6]}}" );
                plan.reset( QueryPlan::make( nsd(),
                                             INDEXNO( "a" << 1 << "b" << 1 ),
                                             FRSP( query ),
                                             FRSP2( query ),
                                             query,
                                             BSONObj() ) );
                cursor = plan->newCursor( DiskLoc(), true );
                ASSERT_NOT_EQUALS( "IntervalBtreeCursor", cursor->toString() );

                // An interval cursor will be created if the field ranges consist of a single
                // interval.
                query = fromjson( "{a:4,b:{$gt:6,$lte:9}}" );
                plan.reset( QueryPlan::make( nsd(),
                                             INDEXNO( "a" << 1 << "b" << 1 ),
                                             FRSP( query ),
                                             FRSP2( query ),
                                             query,
                                             BSONObj() ) );
                cursor = plan->newCursor( DiskLoc(), true );
                ASSERT_EQUALS( "IntervalBtreeCursor", cursor->toString() );
                ASSERT_EQUALS( BSON( "lower" << BSON( "a" << 4 << "b" << 6 ) <<
                                     "upper" << BSON( "a" << 4 << "b" << 9 ) ),
                               cursor->prettyIndexBounds() );

                // But an interval cursor will not be created if it is not requested.
                query = fromjson( "{a:4,b:{$gt:6,$lte:9}}" );
                plan.reset( QueryPlan::make( nsd(),
                                             INDEXNO( "a" << 1 << "b" << 1 ),
                                             FRSP( query ),
                                             FRSP2( query ),
                                             query,
                                             BSONObj() ) );
                cursor = plan->newCursor( DiskLoc(), /* requestIntervalCursor */ false );
                ASSERT_NOT_EQUALS( "IntervalBtreeCursor", cursor->toString() );

                // An interval cursor will not be created for a v0 index (unsupported).
                client().ensureIndex( ns(),
                                      BSON( "x" << 1 << "y" << 1 ),
                                      false,
                                      "",
                                      false,
                                      false,
                                      0 );
                query = fromjson( "{x:2,y:3}" );
                plan.reset( QueryPlan::make( nsd(),
                                             indexno( BSON( "x" << 1 << "y" << 1 ) ),
                                             FRSP( query ),
                                             FRSP2( query ),
                                             query,
                                             BSONObj() ) );
                cursor = plan->newCursor( DiskLoc(), true );
                ASSERT_NOT_EQUALS( "IntervalBtreeCursor", cursor->toString() );
            }
        };

        /**
         * Test that interval cursors returned by newCursor iterate over matching documents only.
         */
        class IntervalCursorBounds : public Base {
        public:
            void run() {
                client().insert( ns(), BSON( "_id" << 0 << "a" << 1 << "b" << 1 ) );
                client().insert( ns(), BSON( "_id" << 1 << "a" << 1 << "b" << 2 ) );
                client().insert( ns(), BSON( "_id" << 2 << "a" << 2 << "b" << 1 ) );
                client().insert( ns(), BSON( "_id" << 3 << "a" << 2 << "b" << 2 ) );

                BSONObj query = fromjson( "{a:2,b:{$lte:2}}" );
                scoped_ptr<QueryPlan> plan( QueryPlan::make( nsd(),
                                                             INDEXNO( "a" << 1 << "b" << 1 ),
                                                             FRSP( query ),
                                                             FRSP2( query ),
                                                             query,
                                                             BSONObj() ) );
                shared_ptr<Cursor> cursor = plan->newCursor( DiskLoc(), true );
                ASSERT_EQUALS( "IntervalBtreeCursor", cursor->toString() );
                ASSERT_EQUALS( BSON( "_id" << 2 << "a" << 2 << "b" << 1 ), cursor->current() );
                ASSERT( cursor->advance() );
                ASSERT_EQUALS( BSON( "_id" << 3 << "a" << 2 << "b" << 2 ), cursor->current() );
                ASSERT( !cursor->advance() );

                query = fromjson( "{a:{$lt:2}}" );
                plan.reset( QueryPlan::make( nsd(),
                                             INDEXNO( "a" << 1 << "b" << 1 ),
                                             FRSP( query ),
                                             FRSP2( query ),
                                             query,
                                             BSONObj() ) );
                cursor = plan->newCursor( DiskLoc(), true );
                ASSERT_EQUALS( "IntervalBtreeCursor", cursor->toString() );
                ASSERT_EQUALS( BSON( "_id" << 0 << "a" << 1 << "b" << 1 ), cursor->current() );
                ASSERT( cursor->advance() );
                ASSERT_EQUALS( BSON( "_id" << 1 << "a" << 1 << "b" << 2 ), cursor->current() );
                ASSERT( !cursor->advance() );

                query = fromjson( "{a:{$lt:2}}" );
                plan.reset( QueryPlan::make( nsd(),
                                             INDEXNO( "a" << 1 << "b" << -1 ), // note -1
                                             FRSP( query ),
                                             FRSP2( query ),
                                             query,
                                             BSONObj() ) );
                cursor = plan->newCursor( DiskLoc(), true );
                ASSERT_EQUALS( "IntervalBtreeCursor", cursor->toString() );
                ASSERT_EQUALS( BSON( "_id" << 1 << "a" << 1 << "b" << 2 ), cursor->current() );
                ASSERT( cursor->advance() );
                ASSERT_EQUALS( BSON( "_id" << 0 << "a" << 1 << "b" << 1 ), cursor->current() );
                ASSERT( !cursor->advance() );

                query = fromjson( "{a:{$gt:1}}" );
                plan.reset( QueryPlan::make( nsd(),
                                             INDEXNO( "a" << 1 << "b" << 1 ),
                                             FRSP( query ),
                                             FRSP2( query ),
                                             query,
                                             BSONObj() ) );
                cursor = plan->newCursor( DiskLoc(), true );
                ASSERT_EQUALS( "IntervalBtreeCursor", cursor->toString() );
                ASSERT_EQUALS( BSON( "_id" << 2 << "a" << 2 << "b" << 1 ), cursor->current() );
                ASSERT( cursor->advance() );
                ASSERT_EQUALS( BSON( "_id" << 3 << "a" << 2 << "b" << 2 ), cursor->current() );
                ASSERT( !cursor->advance() );

                query = fromjson( "{a:{$gt:1}}" );
                plan.reset( QueryPlan::make( nsd(),
                                             INDEXNO( "a" << 1 << "b" << -1 ), // note -1
                                             FRSP( query ),
                                             FRSP2( query ),
                                             query,
                                             BSONObj() ) );
                cursor = plan->newCursor( DiskLoc(), true );
                ASSERT_EQUALS( "IntervalBtreeCursor", cursor->toString() );
                ASSERT_EQUALS( BSON( "_id" << 3 << "a" << 2 << "b" << 2 ), cursor->current() );
                ASSERT( cursor->advance() );
                ASSERT_EQUALS( BSON( "_id" << 2 << "a" << 2 << "b" << 1 ), cursor->current() );
                ASSERT( !cursor->advance() );

                query = fromjson( "{a:2,b:{$lte:2}}" );
                plan.reset( QueryPlan::make( nsd(),
                                             INDEXNO( "a" << 1 << "b" << -1 ), // note -1
                                             FRSP( query ),
                                             FRSP2( query ),
                                             query,
                                             BSONObj() ) );
                cursor = plan->newCursor( DiskLoc(), true );
                ASSERT_EQUALS( "IntervalBtreeCursor", cursor->toString() );
                ASSERT_EQUALS( BSON( "_id" << 3 << "a" << 2 << "b" << 2 ), cursor->current() );
                ASSERT( cursor->advance() );
                ASSERT_EQUALS( BSON( "_id" << 2 << "a" << 2 << "b" << 1 ), cursor->current() );
                ASSERT( !cursor->advance() );

                query = fromjson( "{a:1,b:{$gte:1}}" );
                plan.reset( QueryPlan::make( nsd(),
                                             INDEXNO( "a" << -1 << "b" << -1 ), // note -1
                                             FRSP( query ),
                                             FRSP2( query ),
                                             query,
                                             BSONObj() ) );
                cursor = plan->newCursor( DiskLoc(), true );
                ASSERT_EQUALS( "IntervalBtreeCursor", cursor->toString() );
                ASSERT_EQUALS( BSON( "_id" << 1 << "a" << 1 << "b" << 2 ), cursor->current() );
                ASSERT( cursor->advance() );
                ASSERT_EQUALS( BSON( "_id" << 0 << "a" << 1 << "b" << 1 ), cursor->current() );
                ASSERT( !cursor->advance() );
            }
        };

        /** IntervalBtreeCursor is used and a Matcher is necessary. */
        class IntervalCursorWithMatcher : public Base {
        public:
            void run() {
                client().insert( ns(), BSON( "_id" << 0 << "a" << 1 ) );
                client().insert( ns(), BSON( "_id" << 1 << "a" << 1 << "b" << "exists" ) );
                BSONObj query = BSON( "a" << 1 << "b" << BSON( "$exists" << true ) );
                scoped_ptr<QueryPlan> plan( QueryPlan::make( nsd(),
                                            INDEXNO( "a" << 1 ),
                                            FRSP( query ),
                                            FRSP2( query ),
                                            query,
                                            BSONObj() ) );
                shared_ptr<Cursor> cursor = plan->newCursor( DiskLoc(), true );
                ASSERT_EQUALS( "IntervalBtreeCursor", cursor->toString() );
                ASSERT( plan->mayBeMatcherNecessary() );
                cursor->setMatcher( plan->matcher() );

                // Check the cursor's results, and whether they match.
                ASSERT_EQUALS( 0, cursor->current()[ "_id" ].Int() );
                ASSERT( !cursor->currentMatches() );
                ASSERT( cursor->advance() );
                ASSERT_EQUALS( 1, cursor->current()[ "_id" ].Int() );
                ASSERT( cursor->currentMatches() );
                ASSERT( !cursor->advance() );
            }
        };
        
        namespace QueryBoundsExactOrderSuffix {
            
            class Base : public QueryPlanTests::Base {
            public:
                virtual ~Base() {}
                void run() {
                    BSONObj planQuery = query();
                    BSONObj planOrder = order();
                    scoped_ptr<QueryPlan> plan( QueryPlan::make( nsd(), indexIdx(),
                                                                FRSP( planQuery ),
                                                                FRSP2( planQuery ), planQuery,
                                                                planOrder ) );
                    ASSERT_EQUALS( queryBoundsExactOrderSuffix(),
                                   plan->queryBoundsExactOrderSuffix() );
                }
            protected:
                virtual bool queryBoundsExactOrderSuffix() = 0;
                virtual int indexIdx() { return indexno( index() ); }
                virtual BSONObj index() = 0;
                virtual BSONObj query() = 0;
                virtual BSONObj order() = 0;                
            };
            
            class True : public Base {
                bool queryBoundsExactOrderSuffix() { return true; }
            };
            
            class False : public Base {
                bool queryBoundsExactOrderSuffix() { return false; }
            };
            
            class Unindexed : public False {
                int indexIdx() { return -1; }
                BSONObj index() { return BSON( "wrong" << 1 ); }
                BSONObj query() { return BSON( "a" << 1 ); }
                BSONObj order() { return BSON( "b" << 1 ); }
            };

            class RangeSort : public True {
                BSONObj index() { return BSON( "a" << 1 ); }
                BSONObj query() { return BSON( "a" << GT << 1 ); }
                BSONObj order() { return BSON( "a" << 1 ); }                
            };

            class RangeBeforeSort : public False {
                BSONObj index() { return BSON( "a" << 1 << "b" << 1 ); }
                BSONObj query() { return BSON( "a" << GT << 1 ); }
                BSONObj order() { return BSON( "b" << 1 ); }                
            };

            class EqualityRangeBeforeSort : public False {
                BSONObj index() { return BSON( "a" << 1 << "b" << 1 << "c" << 1 ); }
                BSONObj query() { return BSON( "a" << 1 << "b" << GT << 1 ); }
                BSONObj order() { return BSON( "c" << 1 ); }                
            };

            class EqualSort : public True {
                BSONObj index() { return BSON( "a" << 1 << "b" << 1 ); }
                BSONObj query() { return BSON( "a" << 1 ); }
                BSONObj order() { return BSON( "b" << 1 ); }                
            };

            class InSort : public True {
                BSONObj index() { return BSON( "a" << 1 << "b" << 1 ); }
                BSONObj query() { return fromjson( "{a:{$in:[0,1]}}" ); }
                BSONObj order() { return BSON( "b" << 1 ); }                
            };
            
            class EqualInSort : public True {
                BSONObj index() { return BSON( "a" << 1 << "b" << 1 << "c" << 1 ); }
                BSONObj query() { return fromjson( "{a:10,b:{$in:[0,1]}}" ); }
                BSONObj order() { return BSON( "c" << 1 ); }                
            };
            
            class InInSort : public True {
                BSONObj index() { return BSON( "a" << 1 << "b" << 1 << "c" << 1 ); }
                BSONObj query() { return fromjson( "{a:{$in:[5,6]},b:{$in:[0,1]}}" ); }
                BSONObj order() { return BSON( "c" << 1 ); }
            };
            
            class NonCoveredRange : public False {
                BSONObj index() { return BSON( "a" << 1 << "b" << 1 ); }
                BSONObj query() { return fromjson( "{a:{$in:[5,6]},z:4}" ); }
                BSONObj order() { return BSON( "b" << 1 ); }
            };
            
            class QuerySortOverlap : public True {
                BSONObj index() { return BSON( "a" << 1 << "b" << 1 << "c" << 1 ); }
                BSONObj query() { return fromjson( "{a:10,b:{$in:[0,1]}}" ); }
                BSONObj order() { return BSON( "b" << 1 << "c" << 1 ); }
            };
            
            class OrderDirection : public False {
                BSONObj index() { return BSON( "a" << 1 << "b" << 1 ); }
                BSONObj query() { return fromjson( "{a:{$in:[0,1]}}" ); }
                BSONObj order() { return BSON( "a" << 1 << "b" << -1 ); }
            };
            
            class InterveningIndexField : public False {
                BSONObj index() { return BSON( "a" << 1 << "b" << 1 << "c" << 1 ); }
                BSONObj query() { return fromjson( "{a:{$in:[0,1]}}" ); }
                BSONObj order() { return BSON( "c" << 1 ); }
            };

            class TailingIndexField : public True {
                BSONObj index() { return BSON( "a" << 1 << "b" << 1 << "c" << 1 ); }
                BSONObj query() { return fromjson( "{a:{$in:[0,1]}}" ); }
                BSONObj order() { return BSON( "b" << 1 ); }
            };

            class EmptySort : public True {
                BSONObj index() { return BSON( "a" << 1 << "b" << 1 ); }
                BSONObj query() { return fromjson( "{a:{$in:[0,1]}}" ); }
                BSONObj order() { return BSONObj(); }
            };
            
            class EmptyStringField : public True {
                BSONObj index() { return BSON( "a" << 1 << "" << 1 ); }
                BSONObj query() { return fromjson( "{a:4,'':{$in:[0,1]}}" ); }
                BSONObj order() { return BSONObj(); }                
            };

            class SortedRange : public True {
                BSONObj index() { return BSON( "a" << 1 << "b" << 1 ); }
                BSONObj query() { return fromjson( "{a:{$in:[0,1]},b:{$gt:5}}" ); }
                BSONObj order() { return BSON( "b" << 1 ); }
            };
            
            class SortedRangeWrongDirection : public False {
                BSONObj index() { return BSON( "a" << 1 << "b" << 1 ); }
                BSONObj query() { return fromjson( "{a:{$in:[0,1]},b:{$gt:5}}" ); }
                BSONObj order() { return BSON( "b" << -1 ); }
            };
            
            class SortedDoubleRange : public True {
                BSONObj index() { return BSON( "a" << 1 << "b" << 1 ); }
                BSONObj query() { return fromjson( "{a:{$in:[0,1]},b:{$gt:5,$lt:10}}" ); }
                BSONObj order() { return BSON( "b" << 1 ); }
            };

            class RangeSortPrefix : public True {
                BSONObj index() { return BSON( "a" << 1 << "b" << 1 << "c" << 1 ); }
                BSONObj query() { return fromjson( "{a:{$in:[0,1]},b:{$gt:5}}" ); }
                BSONObj order() { return BSON( "b" << 1 << "c" << 1 ); }
            };
            
            class RangeSortInfix : public True {
                BSONObj index() { return BSON( "a" << 1 << "b" << 1 << "c" << 1 ); }
                BSONObj query() { return fromjson( "{a:{$in:[0,1]},b:{$gt:5}}" ); }
                BSONObj order() { return BSON( "a" << 1 << "b" << 1 << "c" << 1 ); }
            };
            
            class RangeEquality : public False {
                BSONObj index() { return BSON( "a" << 1 << "b" << 1 << "c" << 1 ); }
                BSONObj query() { return fromjson( "{a:{$in:[0,1]},b:{$gt:5},c:2}" ); }
                BSONObj order() { return BSON( "b" << 1 << "c" << 1 ); }
            };

            class RangeRange : public False {
                BSONObj index() { return BSON( "a" << 1 << "b" << 1 << "c" << 1 ); }
                BSONObj query() { return fromjson( "{a:{$in:[0,1]},b:{$gt:5},c:{$gt:2}}" ); }
                BSONObj order() { return BSON( "b" << 1 << "c" << 1 ); }
            };
            
            class Unsatisfiable : public False {
                BSONObj index() { return BSON( "a" << 1 ); }
                BSONObj query() { return fromjson( "{a:{$gt:0,$lt:0}}" ); }
                BSONObj order() { return BSON( "a" << 1 ); }
            };

            class EqualityUnsatisfiable : public False {
                BSONObj index() { return BSON( "a" << 1 << "b" << 1 ); }
                BSONObj query() { return fromjson( "{a:{$in:[0,1]},b:{$gt:0,$lt:0}}" ); }
                BSONObj order() { return BSON( "b" << 1 ); }
            };

        } // namespace QueryBoundsExactOrderSuffix

        /** Checks related to 'special' QueryPlans. */
        class Special : public Base {
        public:
            void run() {
                int idx = INDEXNO( "a" << "2d" );
                BSONObj query = fromjson( "{ a:{ $near:[ 50, 50 ] } }" );
                FieldRangeSetPair frsp( ns(), query );
                scoped_ptr<QueryPlan> plan( QueryPlan::make( nsd(), idx, frsp, FRSP2( query ),
                                                            query, BSONObj(),
                                                            shared_ptr<const ParsedQuery>(),
                                                            BSONObj(), BSONObj(), "2d"));
                // A 'special' plan is not optimal.
                ASSERT_EQUALS( QueryPlan::Helpful, plan->utility() );
            }
        };

    } // namespace QueryPlanTests
    
    class All : public Suite {
    public:
        All() : Suite( "queryoptimizer" ) {}

        void setupTests() {
            add<QueryPlanTests::ToString>();
            add<QueryPlanTests::NoIndex>();
            add<QueryPlanTests::SimpleOrder>();
            add<QueryPlanTests::MoreIndexThanNeeded>();
            add<QueryPlanTests::IndexSigns>();
            add<QueryPlanTests::IndexReverse>();
            add<QueryPlanTests::NoOrder>();
            add<QueryPlanTests::EqualWithOrder>();
            add<QueryPlanTests::Optimal>();
            add<QueryPlanTests::MoreOptimal>();
            add<QueryPlanTests::Impossible>();
            add<QueryPlanTests::NotMatcherNecessary>();
            add<QueryPlanTests::MatcherNecessary>();
            add<QueryPlanTests::MatcherNecessaryMultikey>();
            add<QueryPlanTests::Unhelpful>();
            add<QueryPlanTests::KeyFieldsOnly>();
            add<QueryPlanTests::SparseExistsFalse>();
            add<QueryPlanTests::IntervalCursorCreate>();
            add<QueryPlanTests::IntervalCursorBounds>();
            add<QueryPlanTests::IntervalCursorWithMatcher>();
            add<QueryPlanTests::QueryBoundsExactOrderSuffix::Unindexed>();
            add<QueryPlanTests::QueryBoundsExactOrderSuffix::RangeSort>();
            add<QueryPlanTests::QueryBoundsExactOrderSuffix::RangeBeforeSort>();
            add<QueryPlanTests::QueryBoundsExactOrderSuffix::EqualityRangeBeforeSort>();
            add<QueryPlanTests::QueryBoundsExactOrderSuffix::EqualSort>();
            add<QueryPlanTests::QueryBoundsExactOrderSuffix::InSort>();
            add<QueryPlanTests::QueryBoundsExactOrderSuffix::EqualInSort>();
            add<QueryPlanTests::QueryBoundsExactOrderSuffix::InInSort>();
            add<QueryPlanTests::QueryBoundsExactOrderSuffix::NonCoveredRange>();
            add<QueryPlanTests::QueryBoundsExactOrderSuffix::QuerySortOverlap>();
            add<QueryPlanTests::QueryBoundsExactOrderSuffix::OrderDirection>();
            add<QueryPlanTests::QueryBoundsExactOrderSuffix::InterveningIndexField>();
            add<QueryPlanTests::QueryBoundsExactOrderSuffix::TailingIndexField>();
            add<QueryPlanTests::QueryBoundsExactOrderSuffix::EmptySort>();
            add<QueryPlanTests::QueryBoundsExactOrderSuffix::EmptyStringField>();
            add<QueryPlanTests::QueryBoundsExactOrderSuffix::SortedRange>();
            add<QueryPlanTests::QueryBoundsExactOrderSuffix::SortedRangeWrongDirection>();
            add<QueryPlanTests::QueryBoundsExactOrderSuffix::SortedDoubleRange>();
            add<QueryPlanTests::QueryBoundsExactOrderSuffix::RangeSortPrefix>();
            add<QueryPlanTests::QueryBoundsExactOrderSuffix::RangeSortInfix>();
            add<QueryPlanTests::QueryBoundsExactOrderSuffix::RangeEquality>();
            add<QueryPlanTests::QueryBoundsExactOrderSuffix::RangeRange>();
            add<QueryPlanTests::QueryBoundsExactOrderSuffix::Unsatisfiable>();
            add<QueryPlanTests::QueryBoundsExactOrderSuffix::EqualityUnsatisfiable>();
            add<QueryPlanTests::Special>();
        }
    } myall;

} // namespace QueryOptimizerTests

