// queryoptimizertests.cpp : query optimizer unit tests
//

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

#include "pch.h"
#include "../db/queryoptimizer.h"
#include "../db/instance.h"
#include "../db/ops/count.h"
#include "../db/ops/query.h"
#include "../db/ops/delete.h"
#include "mongo/db/json.h"
#include "dbtests.h"


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

namespace QueryOptimizerTests {

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
                QueryPlan queryPlan( 0, -1, fieldRangeSetPair, 0, obj, order );
                queryPlan.toString(); // Just test that we don't crash.
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
                NamespaceDetails *d = nsd();
                for( int i = 0; i < d->nIndexes; ++i ) {
                    if ( d->idx(i).keyPattern() == key /*indexName() == name*/ || ( d->idx(i).isIdIndex() && IndexDetails::isIdIndexPattern( key ) ) )
                        return &d->idx(i);
                }
                verify( false );
                return 0;
            }
            int indexno( const BSONObj &key ) {
                return nsd()->idxNo( *index(key) );
            }
            BSONObj startKey( const QueryPlan &p ) const {
                return p.frv()->startKey();
            }
            BSONObj endKey( const QueryPlan &p ) const {
                return p.frv()->endKey();
            }
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
                QueryPlan p( nsd(), -1, FRSP( BSONObj() ), FRSP2( BSONObj() ), BSONObj(),
                            BSONObj() );
                ASSERT( !p.optimal() );
                ASSERT( !p.scanAndOrderRequired() );
                ASSERT( !p.exactKeyMatch() );
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

                QueryPlan p( nsd(), INDEXNO( "a" << 1 ), FRSP( BSONObj() ), FRSP2( BSONObj() ),
                            BSONObj(), BSON( "a" << 1 ) );
                ASSERT( !p.scanAndOrderRequired() );
                ASSERT( !startKey( p ).woCompare( start ) );
                ASSERT( !endKey( p ).woCompare( end ) );
                QueryPlan p2( nsd(), INDEXNO( "a" << 1 << "b" << 1 ), FRSP( BSONObj() ),
                             FRSP2( BSONObj() ), BSONObj(),
                             BSON( "a" << 1 << "b" << 1 ) );
                ASSERT( !p2.scanAndOrderRequired() );
                QueryPlan p3( nsd(), INDEXNO( "a" << 1 ), FRSP( BSONObj() ), FRSP2( BSONObj() ),
                             BSONObj(), BSON( "b" << 1 ) );
                ASSERT( p3.scanAndOrderRequired() );
                ASSERT( !startKey( p3 ).woCompare( start ) );
                ASSERT( !endKey( p3 ).woCompare( end ) );
            }
        };

        class MoreIndexThanNeeded : public Base {
        public:
            void run() {
                QueryPlan p( nsd(), INDEXNO( "a" << 1 << "b" << 1 ), FRSP( BSONObj() ),
                            FRSP2( BSONObj() ), BSONObj(),
                            BSON( "a" << 1 ) );
                ASSERT( !p.scanAndOrderRequired() );
            }
        };

        class IndexSigns : public Base {
        public:
            void run() {
                QueryPlan p( nsd(), INDEXNO( "a" << 1 << "b" << -1 ) , FRSP( BSONObj() ),
                            FRSP2( BSONObj() ), BSONObj(),
                            BSON( "a" << 1 << "b" << -1 ) );
                ASSERT( !p.scanAndOrderRequired() );
                ASSERT_EQUALS( 1, p.direction() );
                QueryPlan p2( nsd(), INDEXNO( "a" << 1 << "b" << 1 ), FRSP( BSONObj() ),
                             FRSP2( BSONObj() ), BSONObj(),
                             BSON( "a" << 1 << "b" << -1 ) );
                ASSERT( p2.scanAndOrderRequired() );
                ASSERT_EQUALS( 0, p2.direction() );
                QueryPlan p3( nsd(), indexno( id_obj ), FRSP( BSONObj() ), FRSP2( BSONObj() ),
                             BSONObj(), BSON( "_id" << 1 ) );
                ASSERT( !p3.scanAndOrderRequired() );
                ASSERT_EQUALS( 1, p3.direction() );
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
                QueryPlan p( nsd(),  INDEXNO( "a" << -1 << "b" << 1 ),FRSP( BSONObj() ),
                            FRSP2( BSONObj() ), BSONObj(),
                            BSON( "a" << 1 << "b" << -1 ) );
                ASSERT( !p.scanAndOrderRequired() );
                ASSERT_EQUALS( -1, p.direction() );
                ASSERT( !startKey( p ).woCompare( start ) );
                ASSERT( !endKey( p ).woCompare( end ) );
                QueryPlan p2( nsd(), INDEXNO( "a" << 1 << "b" << 1 ), FRSP( BSONObj() ),
                             FRSP2( BSONObj() ), BSONObj(),
                             BSON( "a" << -1 << "b" << -1 ) );
                ASSERT( !p2.scanAndOrderRequired() );
                ASSERT_EQUALS( -1, p2.direction() );
                QueryPlan p3( nsd(), INDEXNO( "a" << 1 << "b" << -1 ), FRSP( BSONObj() ),
                             FRSP2( BSONObj() ), BSONObj(),
                             BSON( "a" << -1 << "b" << -1 ) );
                ASSERT( p3.scanAndOrderRequired() );
                ASSERT_EQUALS( 0, p3.direction() );
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
                QueryPlan p( nsd(), INDEXNO( "a" << -1 << "b" << 1 ), FRSP( BSON( "a" << 3 ) ),
                            FRSP2( BSON( "a" << 3 ) ), BSON( "a" << 3 ),
                            BSONObj() );
                ASSERT( !p.scanAndOrderRequired() );
                ASSERT( !startKey( p ).woCompare( start ) );
                ASSERT( !endKey( p ).woCompare( end ) );
                QueryPlan p2( nsd(), INDEXNO( "a" << -1 << "b" << 1 ), FRSP( BSON( "a" << 3 ) ),
                             FRSP2( BSON( "a" << 3 ) ), BSON( "a" << 3 ),
                             BSONObj() );
                ASSERT( !p2.scanAndOrderRequired() );
                ASSERT( !startKey( p ).woCompare( start ) );
                ASSERT( !endKey( p ).woCompare( end ) );
            }
        };

        class EqualWithOrder : public Base {
        public:
            void run() {
                QueryPlan p( nsd(), INDEXNO( "a" << 1 << "b" << 1 ), FRSP( BSON( "a" << 4 ) ),
                            FRSP2( BSON( "a" << 4 ) ), BSON( "a" << 4 ),
                            BSON( "b" << 1 ) );
                ASSERT( !p.scanAndOrderRequired() );
                QueryPlan p2( nsd(), INDEXNO( "a" << 1 << "b" << 1 << "c" << 1 ),
                             FRSP( BSON( "b" << 4 ) ), FRSP2( BSON( "b" << 4 ) ),
                             BSON( "b" << 4 ),
                             BSON( "a" << 1 << "c" << 1 ) );
                ASSERT( !p2.scanAndOrderRequired() );
                QueryPlan p3( nsd(), INDEXNO( "a" << 1 << "b" << 1 ), FRSP( BSON( "b" << 4 ) ),
                             FRSP2( BSON( "b" << 4 ) ), BSON( "b" << 4 ),
                             BSON( "a" << 1 << "c" << 1 ) );
                ASSERT( p3.scanAndOrderRequired() );
            }
        };

        class Optimal : public Base {
        public:
            void run() {
                QueryPlan p( nsd(), INDEXNO( "a" << 1 ), FRSP( BSONObj() ), FRSP2( BSONObj() ),
                            BSONObj(), BSON( "a" << 1 ) );
                ASSERT( p.optimal() );
                QueryPlan p2( nsd(), INDEXNO( "a" << 1 << "b" << 1 ), FRSP( BSONObj() ),
                             FRSP2( BSONObj() ), BSONObj(),
                             BSON( "a" << 1 ) );
                ASSERT( p2.optimal() );
                QueryPlan p3( nsd(), INDEXNO( "a" << 1 << "b" << 1 ), FRSP( BSON( "a" << 1 ) ),
                             FRSP2( BSON( "a" << 1 ) ), BSON( "a" << 1 ),
                             BSON( "a" << 1 ) );
                ASSERT( p3.optimal() );
                QueryPlan p4( nsd(), INDEXNO( "a" << 1 << "b" << 1 ), FRSP( BSON( "b" << 1 ) ),
                             FRSP2( BSON( "b" << 1 ) ), BSON( "b" << 1 ),
                             BSON( "a" << 1 ) );
                ASSERT( !p4.optimal() );
                QueryPlan p5( nsd(), INDEXNO( "a" << 1 << "b" << 1 ), FRSP( BSON( "a" << 1 ) ),
                             FRSP2( BSON( "a" << 1 ) ), BSON( "a" << 1 ),
                             BSON( "b" << 1 ) );
                ASSERT( p5.optimal() );
                QueryPlan p6( nsd(), INDEXNO( "a" << 1 << "b" << 1 ), FRSP( BSON( "b" << 1 ) ),
                             FRSP2( BSON( "b" << 1 ) ), BSON( "b" << 1 ),
                             BSON( "b" << 1 ) );
                ASSERT( !p6.optimal() );
                QueryPlan p7( nsd(), INDEXNO( "a" << 1 << "b" << 1 ),
                             FRSP( BSON( "a" << 1 << "b" << 1 ) ),
                             FRSP2( BSON( "a" << 1 << "b" << 1 ) ),
                             BSON( "a" << 1 << "b" << 1 ),
                             BSON( "a" << 1 ) );
                ASSERT( p7.optimal() );
                QueryPlan p8( nsd(), INDEXNO( "a" << 1 << "b" << 1 ),
                             FRSP( BSON( "a" << 1 << "b" << LT << 1 ) ),
                             FRSP2( BSON( "a" << 1 << "b" << LT << 1 ) ),
                             BSON( "a" << 1 << "b" << LT << 1 ),
                             BSON( "a" << 1 )  );
                ASSERT( p8.optimal() );
                QueryPlan p9( nsd(), INDEXNO( "a" << 1 << "b" << 1 << "c" << 1 ),
                             FRSP( BSON( "a" << 1 << "b" << LT << 1 ) ),
                             FRSP2( BSON( "a" << 1 << "b" << LT << 1 ) ),
                             BSON( "a" << 1 << "b" << LT << 1 ),
                             BSON( "a" << 1 ) );
                ASSERT( p9.optimal() );
            }
        };

        class MoreOptimal : public Base {
        public:
            void run() {
                QueryPlan p10( nsd(), INDEXNO( "a" << 1 << "b" << 1 << "c" << 1 ),
                              FRSP( BSON( "a" << 1 ) ), FRSP2( BSON( "a" << 1 ) ), BSON( "a" << 1 ),
                              BSONObj() );
                ASSERT( p10.optimal() );
                QueryPlan p11( nsd(), INDEXNO( "a" << 1 << "b" << 1 << "c" << 1 ),
                              FRSP( BSON( "a" << 1 << "b" << LT << 1 ) ),
                              FRSP2( BSON( "a" << 1 << "b" << LT << 1 ) ),
                              BSON( "a" << 1 << "b" << LT << 1 ),
                              BSONObj() );
                ASSERT( p11.optimal() );
                QueryPlan p12( nsd(), INDEXNO( "a" << 1 << "b" << 1 << "c" << 1 ),
                              FRSP( BSON( "a" << LT << 1 ) ),
                              FRSP2( BSON( "a" << LT << 1 ) ), BSON( "a" << LT << 1 ),
                              BSONObj() );
                ASSERT( p12.optimal() );
                QueryPlan p13( nsd(), INDEXNO( "a" << 1 << "b" << 1 << "c" << 1 ),
                              FRSP( BSON( "a" << LT << 1 ) ), FRSP2( BSON( "a" << LT << 1 ) ),
                              BSON( "a" << LT << 1 ), BSON( "a" << 1 ) );
                ASSERT( p13.optimal() );
                // When no match is possible, optimal attribute is not set.
                BSONObj impossibleQuery = BSON( "a" << BSON( "$in" << BSONArray() ) );
                QueryPlan p14( nsd(), INDEXNO( "a" << 1 ), FRSP( impossibleQuery ),
                             FRSP2( impossibleQuery ), impossibleQuery,
                              BSONObj() );
                ASSERT( !p14.optimal() );
                // When no match is possible on an unindexed field, optimal attribute is not set.
                BSONObj bImpossibleQuery = BSON( "a" << 1 << "b" << GT << 10 << LT << 10 );
                QueryPlan p15( nsd(), INDEXNO( "a" << 1 ), FRSP( bImpossibleQuery ),
                              FRSP2( bImpossibleQuery ), bImpossibleQuery,
                              BSONObj() );
                ASSERT( !p15.optimal() );
            }
        };

        class KeyMatch : public Base {
        public:
            void run() {
                QueryPlan p( nsd(), INDEXNO( "a" << 1 ), FRSP( BSONObj() ), FRSP2( BSONObj() ),
                            BSONObj(), BSON( "a" << 1 ) );
                ASSERT( !p.exactKeyMatch() );
                QueryPlan p2( nsd(), INDEXNO( "b" << 1 << "a" << 1 ), FRSP( BSONObj() ),
                             FRSP2( BSONObj() ), BSONObj(),
                             BSON( "a" << 1 ) );
                ASSERT( !p2.exactKeyMatch() );
                QueryPlan p3( nsd(), INDEXNO( "b" << 1 << "a" << 1 ), FRSP( BSON( "b" << "z" ) ),
                             FRSP2( BSON( "b" << "z" ) ), BSON( "b" << "z" ),
                             BSON( "a" << 1 ) );
                ASSERT( !p3.exactKeyMatch() );
                QueryPlan p4( nsd(), INDEXNO( "b" << 1 << "a" << 1 << "c" << 1 ),
                             FRSP( BSON( "c" << "y" << "b" << "z" ) ),
                             FRSP2( BSON( "c" << "y" << "b" << "z" ) ),
                             BSON( "c" << "y" << "b" << "z" ),
                             BSON( "a" << 1 ) );
                ASSERT( !p4.exactKeyMatch() );
                QueryPlan p5( nsd(), INDEXNO( "b" << 1 << "a" << 1 << "c" << 1 ),
                             FRSP( BSON( "c" << "y" << "b" << "z" ) ),
                             FRSP2( BSON( "c" << "y" << "b" << "z" ) ),
                             BSON( "c" << "y" << "b" << "z" ),
                             BSONObj() );
                ASSERT( !p5.exactKeyMatch() );
                QueryPlan p6( nsd(), INDEXNO( "b" << 1 << "a" << 1 << "c" << 1 ),
                             FRSP( BSON( "c" << LT << "y" << "b" << GT << "z" ) ),
                             FRSP2( BSON( "c" << LT << "y" << "b" << GT << "z" ) ),
                             BSON( "c" << LT << "y" << "b" << GT << "z" ),
                             BSONObj() );
                ASSERT( !p6.exactKeyMatch() );
                QueryPlan p7( nsd(), INDEXNO( "b" << 1 ), FRSP( BSONObj() ), FRSP2( BSONObj() ),
                             BSONObj(), BSON( "a" << 1 ) );
                ASSERT( !p7.exactKeyMatch() );
                QueryPlan p8( nsd(), INDEXNO( "a" << 1 << "b" << 1 ),
                             FRSP( BSON( "b" << "y" << "a" << "z" ) ),
                             FRSP2( BSON( "b" << "y" << "a" << "z" ) ),
                             BSON( "b" << "y" << "a" << "z" ),
                             BSONObj() );
                ASSERT( p8.exactKeyMatch() );
                QueryPlan p9( nsd(), INDEXNO( "a" << 1 ), FRSP( BSON( "a" << "z" ) ),
                             FRSP2( BSON( "a" << "z" ) ), BSON( "a" << "z" ),
                             BSON( "a" << 1 ) );
                ASSERT( p9.exactKeyMatch() );
            }
        };

        class MoreKeyMatch : public Base {
        public:
            void run() {
                QueryPlan p( nsd(), INDEXNO( "a" << 1 ),
                            FRSP( BSON( "a" << "r" << "b" << NE << "q" ) ),
                            FRSP2( BSON( "a" << "r" << "b" << NE << "q" ) ),
                            BSON( "a" << "r" << "b" << NE << "q" ),
                            BSON( "a" << 1 ) );
                ASSERT( !p.exactKeyMatch() );
                // When no match is possible, keyMatch attribute is not set.
                BSONObj impossibleQuery = BSON( "a" << BSON( "$in" << BSONArray() ) );
                QueryPlan p2( nsd(), INDEXNO( "a" << 1 ), FRSP( impossibleQuery ),
                             FRSP2( impossibleQuery ), impossibleQuery,
                             BSONObj() );
                ASSERT( !p2.exactKeyMatch() );
                // When no match is possible on an unindexed field, keyMatch attribute is not set.
                BSONObj bImpossibleQuery = BSON( "a" << 1 << "b" << GT << 10 << LT << 10 );
                QueryPlan p3( nsd(), INDEXNO( "a" << 1 ), FRSP( bImpossibleQuery ),
                              FRSP2( bImpossibleQuery ), bImpossibleQuery,
                             BSONObj() );
                ASSERT( !p3.exactKeyMatch() );
            }
        };

        class ExactKeyQueryTypes : public Base {
        public:
            void run() {
                QueryPlan p( nsd(), INDEXNO( "a" << 1 ), FRSP( BSON( "a" << "b" ) ),
                            FRSP2( BSON( "a" << "b" ) ), BSON( "a" << "b" ),
                            BSONObj() );
                ASSERT( p.exactKeyMatch() );
                QueryPlan p2( nsd(), INDEXNO( "a" << 1 ), FRSP( BSON( "a" << 4 ) ),
                             FRSP2( BSON( "a" << 4 ) ), BSON( "a" << 4 ),
                             BSONObj() );
                ASSERT( !p2.exactKeyMatch() );
                QueryPlan p3( nsd(), INDEXNO( "a" << 1 ), FRSP( BSON( "a" << BSON( "c" << "d" ) ) ),
                             FRSP2( BSON( "a" << BSON( "c" << "d" ) ) ),
                             BSON( "a" << BSON( "c" << "d" ) ),
                             BSONObj() );
                ASSERT( !p3.exactKeyMatch() );
                BSONObjBuilder b;
                b.appendRegex( "a", "^ddd" );
                BSONObj q = b.obj();
                QueryPlan p4( nsd(), INDEXNO( "a" << 1 ), FRSP( q ), FRSP2( q ), q,
                             BSONObj() );
                ASSERT( !p4.exactKeyMatch() );
                QueryPlan p5( nsd(), INDEXNO( "a" << 1 << "b" << 1 ),
                             FRSP( BSON( "a" << "z" << "b" << 4 ) ),
                             FRSP2( BSON( "a" << "z" << "b" << 4 ) ),
                             BSON( "a" << "z" << "b" << 4 ), BSONObj() );
                ASSERT( !p5.exactKeyMatch() );
            }
        };

        class Unhelpful : public Base {
        public:
            void run() {
                QueryPlan p( nsd(), INDEXNO( "a" << 1 << "b" << 1 ), FRSP( BSON( "b" << 1 ) ),
                            FRSP2( BSON( "b" << 1 ) ), BSON( "b" << 1 ),
                            BSONObj() );
                ASSERT( p.multikeyFrs().range( "a" ).universal() );
                ASSERT( p.unhelpful() );
                QueryPlan p2( nsd(), INDEXNO( "a" << 1 << "b" << 1 ),
                             FRSP( BSON( "b" << 1 << "c" << 1 ) ),
                             FRSP2( BSON( "b" << 1 << "c" << 1 ) ),
                             BSON( "b" << 1 << "c" << 1 ),
                             BSON( "a" << 1 ) );
                ASSERT( !p2.scanAndOrderRequired() );
                ASSERT( p2.multikeyFrs().range( "a" ).universal() );
                ASSERT( !p2.unhelpful() );
                QueryPlan p3( nsd(), INDEXNO( "b" << 1 ), FRSP( BSON( "b" << 1 << "c" << 1 ) ),
                             FRSP2( BSON( "b" << 1 << "c" << 1 ) ), BSON( "b" << 1 << "c" << 1 ),
                             BSONObj() );
                ASSERT( !p3.multikeyFrs().range( "b" ).universal() );
                ASSERT( !p3.unhelpful() );
                QueryPlan p4( nsd(), INDEXNO( "b" << 1 << "c" << 1 ),
                             FRSP( BSON( "c" << 1 << "d" << 1 ) ),
                             FRSP2( BSON( "c" << 1 << "d" << 1 ) ), BSON( "c" << 1 << "d" << 1 ),
                             BSONObj() );
                ASSERT( p4.multikeyFrs().range( "b" ).universal() );
                ASSERT( p4.unhelpful() );
            }
        };
        
        class KeyFieldsOnly : public Base {
        public:
            void run() {
                int idx = INDEXNO( "a" << 1 );

                // No fields supplied.
                QueryPlan p( nsd(), idx, FRSP( BSON( "a" << 1 ) ),
                            FRSP2( BSON( "a" << 1 ) ), BSON( "a" << 1 ),
                            BSONObj() );
                ASSERT( !p.keyFieldsOnly() );
                
                // Fields supplied.
                shared_ptr<ParsedQuery> parsedQuery
                        ( new ParsedQuery( ns(), 0, 0, 0, BSONObj(),
                                          BSON( "_id" << 0 << "a" << 1 ) ) );
                QueryPlan p2( nsd(), idx, FRSP( BSON( "a" << 1 ) ),
                             FRSP2( BSON( "a" << 1 ) ), BSON( "a" << 1 ), BSONObj(), parsedQuery );
                ASSERT( p2.keyFieldsOnly() );
                ASSERT_EQUALS( BSON( "a" << 4 ), p2.keyFieldsOnly()->hydrate( BSON( "" << 4 ) ) );
                
                // Fields supplied, but index is multikey.
                DBDirectClient client;
                client.insert( ns(), BSON( "a" << BSON_ARRAY( 1 << 2 ) ) );
                QueryPlan p3( nsd(), idx, FRSP( BSON( "a" << 1 ) ),
                             FRSP2( BSON( "a" << 1 ) ), BSON( "a" << 1 ), BSONObj(), parsedQuery );
                ASSERT( !p3.keyFieldsOnly() );
            }
        };
        
        namespace QueryFiniteSetOrderSuffix {
            
            class Base : public QueryPlanTests::Base {
            public:
                virtual ~Base() {}
                void run() {
                    BSONObj planQuery = query();
                    BSONObj planOrder = order();
                    QueryPlan plan( nsd(), indexIdx(), FRSP( planQuery ), FRSP2( planQuery ),
                                   planQuery, planOrder );
                    ASSERT_EQUALS( queryFiniteSetOrderSuffix(), plan.queryFiniteSetOrderSuffix() );
                }
            protected:
                virtual bool queryFiniteSetOrderSuffix() = 0;
                virtual int indexIdx() { return indexno( index() ); }
                virtual BSONObj index() = 0;
                virtual BSONObj query() = 0;
                virtual BSONObj order() = 0;                
            };
            
            class True : public Base {
                bool queryFiniteSetOrderSuffix() { return true; }
            };
            
            class False : public Base {
                bool queryFiniteSetOrderSuffix() { return false; }
            };
            
            class Unindexed : public False {
                int indexIdx() { return -1; }
                BSONObj index() { return BSON( "wrong" << 1 ); }
                BSONObj query() { return BSON( "a" << 1 ); }
                BSONObj order() { return BSON( "b" << 1 ); }
            };
            
            class RangeQuery : public False {
                BSONObj index() { return BSON( "a" << 1 << "b" << 1 ); }
                BSONObj query() { return BSON( "a" << GT << 1 ); }
                BSONObj order() { return BSON( "b" << 1 ); }                
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
            
        } // namespace QueryFiniteSetOrderSuffix

        /** Checks related to 'special' QueryPlans. */
        class Special : public Base {
        public:
            void run() {
                int idx = INDEXNO( "a" << "2d" );
                BSONObj query = fromjson( "{ a:{ $near:[ 50, 50 ] } }" );
                FieldRangeSetPair frsp( ns(), query );
                QueryPlan plan( nsd(), idx, frsp, FRSP2( query ), query, BSONObj(),
                               shared_ptr<const ParsedQuery>(), BSONObj(), BSONObj(),
                               frsp.getSpecial() );
                // A 'special' plan is not optimal.
                ASSERT( !plan.optimal() );
            }
        };

    } // namespace QueryPlanTests

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
                FieldRangeSet frs( ns(), query, true );
                return QueryPattern( frs, order );
            }
            shared_ptr<QueryPlanSet> makeQps( const BSONObj &query, const BSONObj &order ) {
                auto_ptr< FieldRangeSetPair > frsp( new FieldRangeSetPair( ns(), query ) );
                auto_ptr< FieldRangeSetPair > frspOrig( new FieldRangeSetPair( *frsp ) );
                return shared_ptr<QueryPlanSet>
                ( new QueryPlanSet
                 ( ns(), frsp, frspOrig, query, order ) );
            }
        protected:
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
                BSONObj obj = BSON( "a" << 1 );
                auto_ptr<FieldRangeSetPair> fieldRangeSetPair( new FieldRangeSetPair( ns(), obj ) );
                QueryPlanSet queryPlanSet( ns(), fieldRangeSetPair, auto_ptr<FieldRangeSetPair>(),
                                          obj, BSONObj() );
                queryPlanSet.toString(); // Just test that we don't crash.
            }
        };
        
        class NoIndexes : public Base {
        public:
            void run() {
                auto_ptr< FieldRangeSetPair > frsp( new FieldRangeSetPair( ns(), BSON( "a" << 4 ) ) );
                auto_ptr< FieldRangeSetPair > frspOrig( new FieldRangeSetPair( *frsp ) );
                QueryPlanSet s( ns(), frsp, frspOrig, BSON( "a" << 4 ),
                               BSON( "b" << 1 ) );
                ASSERT_EQUALS( 1, s.nPlans() );
            }
        };

        class Optimal : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), false, "a_1" );
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), false, "b_2" );
                BSONObj query = BSON( "a" << 4 );
                auto_ptr< FieldRangeSetPair > frsp( new FieldRangeSetPair( ns(), query ) );
                auto_ptr< FieldRangeSetPair > frspOrig( new FieldRangeSetPair( *frsp ) );
                QueryPlanSet s( ns(), frsp, frspOrig, query, BSONObj() );

                // Only one optimal plan is added to the plan set.
                ASSERT_EQUALS( 1, s.nPlans() );

                // The optimal plan is recorded in the plan cache.
                FieldRangeSet frs( ns(), query, true );
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
                auto_ptr< FieldRangeSetPair > frsp( new FieldRangeSetPair( ns(), BSON( "a" << 4 ) ) );
                auto_ptr< FieldRangeSetPair > frspOrig( new FieldRangeSetPair( *frsp ) );
                QueryPlanSet s( ns(), frsp, frspOrig, BSON( "a" << 4 ),
                               BSON( "b" << 1 ) );
                ASSERT_EQUALS( 3, s.nPlans() );
            }
        };

        class NoSpec : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), false, "a_1" );
                Helpers::ensureIndex( ns(), BSON( "b" << 1 ), false, "b_1" );
                auto_ptr< FieldRangeSetPair > frsp( new FieldRangeSetPair( ns(), BSONObj() ) );
                auto_ptr< FieldRangeSetPair > frspOrig( new FieldRangeSetPair( *frsp ) );
                QueryPlanSet s( ns(), frsp, frspOrig, BSONObj(),
                               BSONObj() );
                ASSERT_EQUALS( 1, s.nPlans() );
            }
        };

        class HintSpec : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), false, "a_1" );
                Helpers::ensureIndex( ns(), BSON( "b" << 1 ), false, "b_1" );
                BSONObj hint = BSON( "hint" << BSON( "a" << 1 ) );
                auto_ptr< FieldRangeSetPair > frsp( new FieldRangeSetPair( ns(), BSON( "a" << 1 ) ) );
                auto_ptr< FieldRangeSetPair > frspOrig( new FieldRangeSetPair( *frsp ) );
                QueryPlanSet s( ns(), frsp, frspOrig, BSON( "a" << 1 ),
                               BSON( "b" << 1 ), shared_ptr<const ParsedQuery>(), hint );
                ASSERT_EQUALS( 1, s.nPlans() );
            }
        };

        class HintName : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), false, "a_1" );
                Helpers::ensureIndex( ns(), BSON( "b" << 1 ), false, "b_1" );
                BSONObj hint = BSON( "hint" << "a_1" );
                auto_ptr< FieldRangeSetPair > frsp( new FieldRangeSetPair( ns(), BSON( "a" << 1 ) ) );
                auto_ptr< FieldRangeSetPair > frspOrig( new FieldRangeSetPair( *frsp ) );
                QueryPlanSet s( ns(), frsp, frspOrig, BSON( "a" << 1 ),
                               BSON( "b" << 1 ), shared_ptr<const ParsedQuery>(), hint );
                ASSERT_EQUALS( 1, s.nPlans() );
            }
        };

        class NaturalHint : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), false, "a_1" );
                Helpers::ensureIndex( ns(), BSON( "b" << 1 ), false, "b_1" );
                BSONObj hint = BSON( "hint" << BSON( "$natural" << 1 ) );
                auto_ptr< FieldRangeSetPair > frsp( new FieldRangeSetPair( ns(), BSON( "a" << 1 ) ) );
                auto_ptr< FieldRangeSetPair > frspOrig( new FieldRangeSetPair( *frsp ) );
                QueryPlanSet s( ns(), frsp, frspOrig, BSON( "a" << 1 ),
                               BSON( "b" << 1 ), shared_ptr<const ParsedQuery>(), hint );
                ASSERT_EQUALS( 1, s.nPlans() );
            }
        };

        class NaturalSort : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), false, "a_1" );
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), false, "b_2" );
                auto_ptr< FieldRangeSetPair > frsp( new FieldRangeSetPair( ns(), BSON( "a" << 1 ) ) );
                auto_ptr< FieldRangeSetPair > frspOrig( new FieldRangeSetPair( *frsp ) );
                QueryPlanSet s( ns(), frsp, frspOrig, BSON( "a" << 1 ),
                               BSON( "$natural" << 1 ) );
                ASSERT_EQUALS( 1, s.nPlans() );
            }
        };

        class BadHint : public Base {
        public:
            void run() {
                BSONObj hint = BSON( "hint" << "a_1" );
                auto_ptr< FieldRangeSetPair > frsp( new FieldRangeSetPair( ns(), BSON( "a" << 1 ) ) );
                auto_ptr< FieldRangeSetPair > frspOrig( new FieldRangeSetPair( *frsp ) );
                ASSERT_THROWS( QueryPlanSet s( ns(), frsp, frspOrig, BSON( "a" << 1 ),
                                              BSON( "b" << 1 ), shared_ptr<const ParsedQuery>(),
                                              hint ),
                                  AssertionException );
            }
        };

        class Count : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), false, "a_1" );
                Helpers::ensureIndex( ns(), BSON( "b" << 1 ), false, "b_1" );
                string err;
                ASSERT_EQUALS( 0, runCount( ns(), BSON( "query" << BSON( "a" << 4 ) ), err ) );
                BSONObj one = BSON( "a" << 1 );
                BSONObj fourA = BSON( "a" << 4 );
                BSONObj fourB = BSON( "a" << 4 );
                theDataFileMgr.insertWithObjMod( ns(), one );
                ASSERT_EQUALS( 0, runCount( ns(), BSON( "query" << BSON( "a" << 4 ) ), err ) );
                theDataFileMgr.insertWithObjMod( ns(), fourA );
                ASSERT_EQUALS( 1, runCount( ns(), BSON( "query" << BSON( "a" << 4 ) ), err ) );
                theDataFileMgr.insertWithObjMod( ns(), fourB );
                ASSERT_EQUALS( 2, runCount( ns(), BSON( "query" << BSON( "a" << 4 ) ), err ) );
                ASSERT_EQUALS( 3, runCount( ns(), BSON( "query" << BSONObj() ), err ) );
                ASSERT_EQUALS( 3, runCount( ns(), BSON( "query" << BSON( "a" << GT << 0 ) ), err ) );
                // missing ns
                ASSERT_EQUALS( -1, runCount( "unittests.missingNS", BSONObj(), err ) );
                // impossible match
                ASSERT_EQUALS( 0, runCount( ns(), BSON( "query" << BSON( "a" << GT << 0 << LT << -1 ) ), err ) );
            }
        };

        class QueryMissingNs : public Base {
        public:
            QueryMissingNs() { log() << "querymissingns starts" << endl; }
            ~QueryMissingNs() {
                log() << "end QueryMissingNs" << endl;
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
                auto_ptr< FieldRangeSetPair > frsp( new FieldRangeSetPair( ns(), BSON( "a" << 1 << "c" << 2 ) ) );
                auto_ptr< FieldRangeSetPair > frspOrig( new FieldRangeSetPair( *frsp ) );
                QueryPlanSet s( ns(), frsp, frspOrig, BSON( "a" << 1 << "c" << 2 ),
                               BSONObj() );
                ASSERT_EQUALS( 2, s.nPlans() );
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
                QueryPattern queryPattern = FieldRangeSet( ns(), delSpec, true ).pattern();
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
                BSONObj hint = fromjson( "{$hint:{a:1}}" );
                BSONObj query = fromjson( "{a:{$in:[2,3,6,9,11]}}" );
                BSONObj order = BSONObj();
                auto_ptr< FieldRangeSetPair > frsp( new FieldRangeSetPair( ns(), query ) );
                auto_ptr< FieldRangeSetPair > frspOrig( new FieldRangeSetPair( *frsp ) );
                auto_ptr< FieldRangeSetPair > frspOrig2( new FieldRangeSetPair( *frsp ) );
                QueryPlanSet s( ns(), frsp, frspOrig, query, order,
                               shared_ptr<const ParsedQuery>(), hint );
                QueryPlan qp( nsd(), 1, s.frsp(), frspOrig2.get(), query, order );
                boost::shared_ptr<Cursor> c = qp.newCursor();
                double expected[] = { 2, 3, 6, 9 };
                for( int i = 0; i < 4; ++i, c->advance() ) {
                    ASSERT_EQUALS( expected[ i ], c->current().getField( "a" ).number() );
                }
                ASSERT( !c->ok() );

                // now check reverse
                {
                    order = BSON( "a" << -1 );
                    auto_ptr< FieldRangeSetPair > frsp( new FieldRangeSetPair( ns(), query ) );
                    auto_ptr< FieldRangeSetPair > frspOrig( new FieldRangeSetPair( *frsp ) );
                    auto_ptr< FieldRangeSetPair > frspOrig2( new FieldRangeSetPair( *frsp ) );
                    QueryPlanSet s( ns(), frsp, frspOrig, query, order,
                                   shared_ptr<const ParsedQuery>(), hint );
                    QueryPlan qp( nsd(), 1, s.frsp(), frspOrig2.get(), query, order );
                    boost::shared_ptr<Cursor> c = qp.newCursor();
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
                QueryPlan qp( nsd(), 1, *frsp, frsp.get(), fromjson( "{a:5,b:{$in:[2,3,6,9,11]}}" ),
                             BSONObj() );
                boost::shared_ptr<Cursor> c = qp.newCursor();
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
                QueryPlan qp( nsd(), 1, *frsp, frsp.get(),
                             fromjson( "{a:{$gte:5},b:{$in:[2,3,6,9,11]}}" ),
                             BSONObj() );
                boost::shared_ptr<Cursor> c = qp.newCursor();
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
                BSONObj query = BSON( "a" << BSON_ARRAY( 0 << 0 ) << "b" << 1 );
                auto_ptr< FieldRangeSetPair > frsp( new FieldRangeSetPair( ns(), query ) );
                auto_ptr< FieldRangeSetPair > frspOrig( new FieldRangeSetPair( *frsp ) );
                QueryPlanSet s( ns(), frsp, frspOrig, query, BSONObj() );
                // Two query plans, btree and collection scan.
                ASSERT_EQUALS( 2, s.nPlans() );
                // Not the geo plan.
                ASSERT( s.firstPlan()->special().empty() );
            }
        };
        
        /** Exclude unindexed plan candidate if there is a special plan candidate. SERVER-4531 */
        class ExcludeUnindexedPlanWhenSpecialPlan : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << "2d" ), false, "a_2d" );
                BSONObj query = BSON( "a" << BSON_ARRAY( 0 << 0 ) << "b" << 1 );
                auto_ptr< FieldRangeSetPair > frsp( new FieldRangeSetPair( ns(), query ) );
                auto_ptr< FieldRangeSetPair > frspOrig( new FieldRangeSetPair( *frsp ) );
                QueryPlanSet s( ns(), frsp, frspOrig, query, BSONObj() );
                // Single query plan.
                ASSERT_EQUALS( 1, s.nPlans() );
                // It's the geo plan.
                ASSERT( !s.firstPlan()->special().empty() );                
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
            FieldRangeSet frs( ns(), query, true );
            return QueryPattern( frs, order );
        }
        shared_ptr<MultiPlanScanner> makeMps( const BSONObj &query, const BSONObj &order ) {
            return shared_ptr<MultiPlanScanner>
            ( new MultiPlanScanner
             ( ns(), query, order ) );
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
                MultiPlanScanner multiPlanScanner( ns(), BSON( "a" << 1 ), BSONObj() );
                multiPlanScanner.toString(); // Just test that we don't crash.
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
                    makeMps( fromjson( "{$or:[{a:1}]}" ), BSON( "c" << 1 ) );
                    ASSERT_EQUALS( 1, mps->currentNPlans() );
                    ASSERT( !mps->possibleInOrderPlan() );
                    ASSERT( !mps->haveInOrderPlan() );
                    ASSERT( mps->possibleOutOfOrderPlan() );
                    ASSERT( !mps->hasPossiblyExcludedPlans() );
                }

                {
                    shared_ptr<MultiPlanScanner> mps =
                    makeMps( fromjson( "{$or:[{a:1,b:1}]}" ), BSONObj() );
                    ASSERT_EQUALS( 0, mps->currentNPlans() );
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

            boost::shared_ptr< Cursor > c =
            NamespaceDetailsTransient::bestGuessCursor( ns(), BSON( "b" << 1 ), BSON( "a" << 1 ) );
            ASSERT_EQUALS( string( "a" ), c->indexKeyPattern().firstElement().fieldName() );
            
            c = NamespaceDetailsTransient::bestGuessCursor( ns(), BSON( "a" << 1 ),
                                                           BSON( "b" << 1 ) );
            ASSERT_EQUALS( string( "b" ), c->indexKeyPattern().firstElementFieldName() );
            
            c = NamespaceDetailsTransient::bestGuessCursor( ns(), fromjson( "{b:1,$or:[{z:1}]}" ),
                                                         BSON( "a" << 1 ) );
            ASSERT_EQUALS( string( "a" ), c->indexKeyPattern().firstElement().fieldName() );

            c = NamespaceDetailsTransient::bestGuessCursor( ns(), fromjson( "{a:1,$or:[{y:1}]}" ),
                                                         BSON( "b" << 1 ) );
            ASSERT_EQUALS( string( "b" ), c->indexKeyPattern().firstElementFieldName() );

            FieldRangeSet frs( "ns", BSON( "a" << 1 ), true );
            {
                SimpleMutex::scoped_lock lk(NamespaceDetailsTransient::_qcMutex);
                NamespaceDetailsTransient::get_inlock( ns() ).
                        registerCachedQueryPlanForPattern( frs.pattern( BSON( "b" << 1 ) ),
                                                          CachedQueryPlan( BSON( "a" << 1 ), 0,
                                                        CandidatePlanCharacter( true, true ) ) );
            }
            
            c = NamespaceDetailsTransient::bestGuessCursor( ns(), fromjson( "{a:1,$or:[{y:1}]}" ),
                                                           BSON( "b" << 1 ) );
            ASSERT_EQUALS( string( "b" ),
                          c->indexKeyPattern().firstElement().fieldName() );
        }
    };
    
    class All : public Suite {
    public:
        All() : Suite( "queryoptimizer" ) {}

        void setupTests() {
            __forceLinkGeoPlugin();
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
            add<QueryPlanTests::KeyMatch>();
            add<QueryPlanTests::MoreKeyMatch>();
            add<QueryPlanTests::ExactKeyQueryTypes>();
            add<QueryPlanTests::Unhelpful>();
            add<QueryPlanTests::KeyFieldsOnly>();
            add<QueryPlanTests::QueryFiniteSetOrderSuffix::Unindexed>();
            add<QueryPlanTests::QueryFiniteSetOrderSuffix::RangeQuery>();
            add<QueryPlanTests::QueryFiniteSetOrderSuffix::EqualSort>();
            add<QueryPlanTests::QueryFiniteSetOrderSuffix::InSort>();
            add<QueryPlanTests::QueryFiniteSetOrderSuffix::EqualInSort>();
            add<QueryPlanTests::QueryFiniteSetOrderSuffix::InInSort>();
            add<QueryPlanTests::QueryFiniteSetOrderSuffix::NonCoveredRange>();
            add<QueryPlanTests::QueryFiniteSetOrderSuffix::QuerySortOverlap>();
            add<QueryPlanTests::QueryFiniteSetOrderSuffix::OrderDirection>();
            add<QueryPlanTests::QueryFiniteSetOrderSuffix::InterveningIndexField>();
            add<QueryPlanTests::QueryFiniteSetOrderSuffix::TailingIndexField>();
            add<QueryPlanTests::QueryFiniteSetOrderSuffix::EmptySort>();
            add<QueryPlanTests::QueryFiniteSetOrderSuffix::EmptyStringField>();
            add<QueryPlanTests::Special>();
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
            add<MultiPlanScannerTests::ToString>();
            add<MultiPlanScannerTests::PossiblePlans>();
            add<BestGuess>();
        }
    } myall;

} // namespace QueryOptimizerTests

