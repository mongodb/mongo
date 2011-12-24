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

    void dropCollection( const char *ns ) {
     	string errmsg;
        BSONObjBuilder result;
        dropCollection( ns, errmsg, result );
    }
    
    namespace QueryPlanTests {

        using boost::shared_ptr;

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
                assert( false );
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
            dblock lk_;
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
                QueryPlan p( nsd(), -1, FRSP( BSONObj() ), FRSP2( BSONObj() ), BSONObj(), BSONObj() );
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

                QueryPlan p( nsd(), INDEXNO( "a" << 1 ), FRSP( BSONObj() ), FRSP2( BSONObj() ), BSONObj(), BSON( "a" << 1 ) );
                ASSERT( !p.scanAndOrderRequired() );
                ASSERT( !startKey( p ).woCompare( start ) );
                ASSERT( !endKey( p ).woCompare( end ) );
                QueryPlan p2( nsd(), INDEXNO( "a" << 1 << "b" << 1 ), FRSP( BSONObj() ), FRSP2( BSONObj() ), BSONObj(), BSON( "a" << 1 << "b" << 1 ) );
                ASSERT( !p2.scanAndOrderRequired() );
                QueryPlan p3( nsd(), INDEXNO( "a" << 1 ), FRSP( BSONObj() ), FRSP2( BSONObj() ), BSONObj(), BSON( "b" << 1 ) );
                ASSERT( p3.scanAndOrderRequired() );
                ASSERT( !startKey( p3 ).woCompare( start ) );
                ASSERT( !endKey( p3 ).woCompare( end ) );
            }
        };

        class MoreIndexThanNeeded : public Base {
        public:
            void run() {
                QueryPlan p( nsd(), INDEXNO( "a" << 1 << "b" << 1 ), FRSP( BSONObj() ), FRSP2( BSONObj() ), BSONObj(), BSON( "a" << 1 ) );
                ASSERT( !p.scanAndOrderRequired() );
            }
        };

        class IndexSigns : public Base {
        public:
            void run() {
                QueryPlan p( nsd(), INDEXNO( "a" << 1 << "b" << -1 ) , FRSP( BSONObj() ), FRSP2( BSONObj() ), BSONObj(), BSON( "a" << 1 << "b" << -1 ) );
                ASSERT( !p.scanAndOrderRequired() );
                ASSERT_EQUALS( 1, p.direction() );
                QueryPlan p2( nsd(), INDEXNO( "a" << 1 << "b" << 1 ), FRSP( BSONObj() ), FRSP2( BSONObj() ), BSONObj(), BSON( "a" << 1 << "b" << -1 ) );
                ASSERT( p2.scanAndOrderRequired() );
                ASSERT_EQUALS( 0, p2.direction() );
                QueryPlan p3( nsd(), indexno( id_obj ), FRSP( BSONObj() ), FRSP2( BSONObj() ), BSONObj(), BSON( "_id" << 1 ) );
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
                QueryPlan p( nsd(),  INDEXNO( "a" << -1 << "b" << 1 ),FRSP( BSONObj() ), FRSP2( BSONObj() ), BSONObj(), BSON( "a" << 1 << "b" << -1 ) );
                ASSERT( !p.scanAndOrderRequired() );
                ASSERT_EQUALS( -1, p.direction() );
                ASSERT( !startKey( p ).woCompare( start ) );
                ASSERT( !endKey( p ).woCompare( end ) );
                QueryPlan p2( nsd(), INDEXNO( "a" << 1 << "b" << 1 ), FRSP( BSONObj() ), FRSP2( BSONObj() ), BSONObj(), BSON( "a" << -1 << "b" << -1 ) );
                ASSERT( !p2.scanAndOrderRequired() );
                ASSERT_EQUALS( -1, p2.direction() );
                QueryPlan p3( nsd(), INDEXNO( "a" << 1 << "b" << -1 ), FRSP( BSONObj() ), FRSP2( BSONObj() ), BSONObj(), BSON( "a" << -1 << "b" << -1 ) );
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
                QueryPlan p( nsd(), INDEXNO( "a" << -1 << "b" << 1 ), FRSP( BSON( "a" << 3 ) ), FRSP2( BSON( "a" << 3 ) ), BSON( "a" << 3 ), BSONObj() );
                ASSERT( !p.scanAndOrderRequired() );
                ASSERT( !startKey( p ).woCompare( start ) );
                ASSERT( !endKey( p ).woCompare( end ) );
                QueryPlan p2( nsd(), INDEXNO( "a" << -1 << "b" << 1 ), FRSP( BSON( "a" << 3 ) ), FRSP2( BSON( "a" << 3 ) ), BSON( "a" << 3 ), BSONObj() );
                ASSERT( !p2.scanAndOrderRequired() );
                ASSERT( !startKey( p ).woCompare( start ) );
                ASSERT( !endKey( p ).woCompare( end ) );
            }
        };

        class EqualWithOrder : public Base {
        public:
            void run() {
                QueryPlan p( nsd(), INDEXNO( "a" << 1 << "b" << 1 ), FRSP( BSON( "a" << 4 ) ), FRSP2( BSON( "a" << 4 ) ), BSON( "a" << 4 ), BSON( "b" << 1 ) );
                ASSERT( !p.scanAndOrderRequired() );
                QueryPlan p2( nsd(), INDEXNO( "a" << 1 << "b" << 1 << "c" << 1 ), FRSP( BSON( "b" << 4 ) ), FRSP2( BSON( "b" << 4 ) ), BSON( "b" << 4 ), BSON( "a" << 1 << "c" << 1 ) );
                ASSERT( !p2.scanAndOrderRequired() );
                QueryPlan p3( nsd(), INDEXNO( "a" << 1 << "b" << 1 ), FRSP( BSON( "b" << 4 ) ), FRSP2( BSON( "b" << 4 ) ), BSON( "b" << 4 ), BSON( "a" << 1 << "c" << 1 ) );
                ASSERT( p3.scanAndOrderRequired() );
            }
        };

        class Optimal : public Base {
        public:
            void run() {
                QueryPlan p( nsd(), INDEXNO( "a" << 1 ), FRSP( BSONObj() ), FRSP2( BSONObj() ), BSONObj(), BSON( "a" << 1 ) );
                ASSERT( p.optimal() );
                QueryPlan p2( nsd(), INDEXNO( "a" << 1 << "b" << 1 ), FRSP( BSONObj() ), FRSP2( BSONObj() ), BSONObj(), BSON( "a" << 1 ) );
                ASSERT( p2.optimal() );
                QueryPlan p3( nsd(), INDEXNO( "a" << 1 << "b" << 1 ), FRSP( BSON( "a" << 1 ) ), FRSP2( BSON( "a" << 1 ) ), BSON( "a" << 1 ), BSON( "a" << 1 ) );
                ASSERT( p3.optimal() );
                QueryPlan p4( nsd(), INDEXNO( "a" << 1 << "b" << 1 ), FRSP( BSON( "b" << 1 ) ), FRSP2( BSON( "b" << 1 ) ), BSON( "b" << 1 ), BSON( "a" << 1 ) );
                ASSERT( !p4.optimal() );
                QueryPlan p5( nsd(), INDEXNO( "a" << 1 << "b" << 1 ), FRSP( BSON( "a" << 1 ) ), FRSP2( BSON( "a" << 1 ) ), BSON( "a" << 1 ), BSON( "b" << 1 ) );
                ASSERT( p5.optimal() );
                QueryPlan p6( nsd(), INDEXNO( "a" << 1 << "b" << 1 ), FRSP( BSON( "b" << 1 ) ), FRSP2( BSON( "b" << 1 ) ), BSON( "b" << 1 ), BSON( "b" << 1 ) );
                ASSERT( !p6.optimal() );
                QueryPlan p7( nsd(), INDEXNO( "a" << 1 << "b" << 1 ), FRSP( BSON( "a" << 1 << "b" << 1 ) ), FRSP2( BSON( "a" << 1 << "b" << 1 ) ), BSON( "a" << 1 << "b" << 1 ), BSON( "a" << 1 ) );
                ASSERT( p7.optimal() );
                QueryPlan p8( nsd(), INDEXNO( "a" << 1 << "b" << 1 ), FRSP( BSON( "a" << 1 << "b" << LT << 1 ) ), FRSP2( BSON( "a" << 1 << "b" << LT << 1 ) ), BSON( "a" << 1 << "b" << LT << 1 ), BSON( "a" << 1 )  );
                ASSERT( p8.optimal() );
                QueryPlan p9( nsd(), INDEXNO( "a" << 1 << "b" << 1 << "c" << 1 ), FRSP( BSON( "a" << 1 << "b" << LT << 1 ) ), FRSP2( BSON( "a" << 1 << "b" << LT << 1 ) ), BSON( "a" << 1 << "b" << LT << 1 ), BSON( "a" << 1 ) );
                ASSERT( p9.optimal() );
            }
        };

        class MoreOptimal : public Base {
        public:
            void run() {
                QueryPlan p10( nsd(), INDEXNO( "a" << 1 << "b" << 1 << "c" << 1 ), FRSP( BSON( "a" << 1 ) ), FRSP2( BSON( "a" << 1 ) ), BSON( "a" << 1 ), BSONObj() );
                ASSERT( p10.optimal() );
                QueryPlan p11( nsd(), INDEXNO( "a" << 1 << "b" << 1 << "c" << 1 ), FRSP( BSON( "a" << 1 << "b" << LT << 1 ) ), FRSP2( BSON( "a" << 1 << "b" << LT << 1 ) ), BSON( "a" << 1 << "b" << LT << 1 ), BSONObj() );
                ASSERT( p11.optimal() );
                QueryPlan p12( nsd(), INDEXNO( "a" << 1 << "b" << 1 << "c" << 1 ), FRSP( BSON( "a" << LT << 1 ) ), FRSP2( BSON( "a" << LT << 1 ) ), BSON( "a" << LT << 1 ), BSONObj() );
                ASSERT( p12.optimal() );
                QueryPlan p13( nsd(), INDEXNO( "a" << 1 << "b" << 1 << "c" << 1 ), FRSP( BSON( "a" << LT << 1 ) ), FRSP2( BSON( "a" << LT << 1 ) ), BSON( "a" << LT << 1 ), BSON( "a" << 1 ) );
                ASSERT( p13.optimal() );
            }
        };

        class KeyMatch : public Base {
        public:
            void run() {
                QueryPlan p( nsd(), INDEXNO( "a" << 1 ), FRSP( BSONObj() ), FRSP2( BSONObj() ), BSONObj(), BSON( "a" << 1 ) );
                ASSERT( !p.exactKeyMatch() );
                QueryPlan p2( nsd(), INDEXNO( "b" << 1 << "a" << 1 ), FRSP( BSONObj() ), FRSP2( BSONObj() ), BSONObj(), BSON( "a" << 1 ) );
                ASSERT( !p2.exactKeyMatch() );
                QueryPlan p3( nsd(), INDEXNO( "b" << 1 << "a" << 1 ), FRSP( BSON( "b" << "z" ) ), FRSP2( BSON( "b" << "z" ) ), BSON( "b" << "z" ), BSON( "a" << 1 ) );
                ASSERT( !p3.exactKeyMatch() );
                QueryPlan p4( nsd(), INDEXNO( "b" << 1 << "a" << 1 << "c" << 1 ), FRSP( BSON( "c" << "y" << "b" << "z" ) ), FRSP2( BSON( "c" << "y" << "b" << "z" ) ), BSON( "c" << "y" << "b" << "z" ), BSON( "a" << 1 ) );
                ASSERT( !p4.exactKeyMatch() );
                QueryPlan p5( nsd(), INDEXNO( "b" << 1 << "a" << 1 << "c" << 1 ), FRSP( BSON( "c" << "y" << "b" << "z" ) ), FRSP2( BSON( "c" << "y" << "b" << "z" ) ), BSON( "c" << "y" << "b" << "z" ), BSONObj() );
                ASSERT( !p5.exactKeyMatch() );
                QueryPlan p6( nsd(), INDEXNO( "b" << 1 << "a" << 1 << "c" << 1 ), FRSP( BSON( "c" << LT << "y" << "b" << GT << "z" ) ), FRSP2( BSON( "c" << LT << "y" << "b" << GT << "z" ) ), BSON( "c" << LT << "y" << "b" << GT << "z" ), BSONObj() );
                ASSERT( !p6.exactKeyMatch() );
                QueryPlan p7( nsd(), INDEXNO( "b" << 1 ), FRSP( BSONObj() ), FRSP2( BSONObj() ), BSONObj(), BSON( "a" << 1 ) );
                ASSERT( !p7.exactKeyMatch() );
                QueryPlan p8( nsd(), INDEXNO( "a" << 1 << "b" << 1 ), FRSP( BSON( "b" << "y" << "a" << "z" ) ), FRSP2( BSON( "b" << "y" << "a" << "z" ) ), BSON( "b" << "y" << "a" << "z" ), BSONObj() );
                ASSERT( p8.exactKeyMatch() );
                QueryPlan p9( nsd(), INDEXNO( "a" << 1 ), FRSP( BSON( "a" << "z" ) ), FRSP2( BSON( "a" << "z" ) ), BSON( "a" << "z" ), BSON( "a" << 1 ) );
                ASSERT( p9.exactKeyMatch() );
            }
        };

        class MoreKeyMatch : public Base {
        public:
            void run() {
                QueryPlan p( nsd(), INDEXNO( "a" << 1 ), FRSP( BSON( "a" << "r" << "b" << NE << "q" ) ), FRSP2( BSON( "a" << "r" << "b" << NE << "q" ) ), BSON( "a" << "r" << "b" << NE << "q" ), BSON( "a" << 1 ) );
                ASSERT( !p.exactKeyMatch() );
            }
        };

        class ExactKeyQueryTypes : public Base {
        public:
            void run() {
                QueryPlan p( nsd(), INDEXNO( "a" << 1 ), FRSP( BSON( "a" << "b" ) ), FRSP2( BSON( "a" << "b" ) ), BSON( "a" << "b" ), BSONObj() );
                ASSERT( p.exactKeyMatch() );
                QueryPlan p2( nsd(), INDEXNO( "a" << 1 ), FRSP( BSON( "a" << 4 ) ), FRSP2( BSON( "a" << 4 ) ), BSON( "a" << 4 ), BSONObj() );
                ASSERT( !p2.exactKeyMatch() );
                QueryPlan p3( nsd(), INDEXNO( "a" << 1 ), FRSP( BSON( "a" << BSON( "c" << "d" ) ) ), FRSP2( BSON( "a" << BSON( "c" << "d" ) ) ), BSON( "a" << BSON( "c" << "d" ) ), BSONObj() );
                ASSERT( !p3.exactKeyMatch() );
                BSONObjBuilder b;
                b.appendRegex( "a", "^ddd" );
                BSONObj q = b.obj();
                QueryPlan p4( nsd(), INDEXNO( "a" << 1 ), FRSP( q ), FRSP2( q ), q, BSONObj() );
                ASSERT( !p4.exactKeyMatch() );
                QueryPlan p5( nsd(), INDEXNO( "a" << 1 << "b" << 1 ), FRSP( BSON( "a" << "z" << "b" << 4 ) ), FRSP2( BSON( "a" << "z" << "b" << 4 ) ), BSON( "a" << "z" << "b" << 4 ), BSONObj() );
                ASSERT( !p5.exactKeyMatch() );
            }
        };

        class Unhelpful : public Base {
        public:
            void run() {
                QueryPlan p( nsd(), INDEXNO( "a" << 1 << "b" << 1 ), FRSP( BSON( "b" << 1 ) ), FRSP2( BSON( "b" << 1 ) ), BSON( "b" << 1 ), BSONObj() );
                ASSERT( !p.range( "a" ).nontrivial() );
                ASSERT( p.unhelpful() );
                QueryPlan p2( nsd(), INDEXNO( "a" << 1 << "b" << 1 ), FRSP( BSON( "b" << 1 << "c" << 1 ) ), FRSP2( BSON( "b" << 1 << "c" << 1 ) ), BSON( "b" << 1 << "c" << 1 ), BSON( "a" << 1 ) );
                ASSERT( !p2.scanAndOrderRequired() );
                ASSERT( !p2.range( "a" ).nontrivial() );
                ASSERT( !p2.unhelpful() );
                QueryPlan p3( nsd(), INDEXNO( "b" << 1 ), FRSP( BSON( "b" << 1 << "c" << 1 ) ), FRSP2( BSON( "b" << 1 << "c" << 1 ) ), BSON( "b" << 1 << "c" << 1 ), BSONObj() );
                ASSERT( p3.range( "b" ).nontrivial() );
                ASSERT( !p3.unhelpful() );
                QueryPlan p4( nsd(), INDEXNO( "b" << 1 << "c" << 1 ), FRSP( BSON( "c" << 1 << "d" << 1 ) ), FRSP2( BSON( "c" << 1 << "d" << 1 ) ), BSON( "c" << 1 << "d" << 1 ), BSONObj() );
                ASSERT( !p4.range( "b" ).nontrivial() );
                ASSERT( p4.unhelpful() );
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
        protected:
            static const char *ns() { return "unittests.QueryPlanSetTests"; }
            static NamespaceDetails *nsd() { return nsdetails( ns() ); }
        private:
            dblock lk_;
            Client::Context _context;
        };

        class NoIndexes : public Base {
        public:
            void run() {
                auto_ptr< FieldRangeSetPair > frsp( new FieldRangeSetPair( ns(), BSON( "a" << 4 ) ) );
                auto_ptr< FieldRangeSetPair > frspOrig( new FieldRangeSetPair( *frsp ) );
                QueryPlanSet s( ns(), frsp, frspOrig, BSON( "a" << 4 ), BSON( "b" << 1 ) );
                ASSERT_EQUALS( 1, s.nPlans() );
            }
        };

        class Optimal : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), false, "a_1" );
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), false, "b_2" );
                auto_ptr< FieldRangeSetPair > frsp( new FieldRangeSetPair( ns(), BSON( "a" << 4 ) ) );
                auto_ptr< FieldRangeSetPair > frspOrig( new FieldRangeSetPair( *frsp ) );
                QueryPlanSet s( ns(), frsp, frspOrig, BSON( "a" << 4 ), BSONObj() );
                ASSERT_EQUALS( 1, s.nPlans() );
            }
        };

        class NoOptimal : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), false, "a_1" );
                Helpers::ensureIndex( ns(), BSON( "b" << 1 ), false, "b_1" );
                auto_ptr< FieldRangeSetPair > frsp( new FieldRangeSetPair( ns(), BSON( "a" << 4 ) ) );
                auto_ptr< FieldRangeSetPair > frspOrig( new FieldRangeSetPair( *frsp ) );
                QueryPlanSet s( ns(), frsp, frspOrig, BSON( "a" << 4 ), BSON( "b" << 1 ) );
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
                QueryPlanSet s( ns(), frsp, frspOrig, BSONObj(), BSONObj() );
                ASSERT_EQUALS( 1, s.nPlans() );
            }
        };

        class HintSpec : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), false, "a_1" );
                Helpers::ensureIndex( ns(), BSON( "b" << 1 ), false, "b_1" );
                BSONObj b = BSON( "hint" << BSON( "a" << 1 ) );
                BSONElement e = b.firstElement();
                auto_ptr< FieldRangeSetPair > frsp( new FieldRangeSetPair( ns(), BSON( "a" << 1 ) ) );
                auto_ptr< FieldRangeSetPair > frspOrig( new FieldRangeSetPair( *frsp ) );
                QueryPlanSet s( ns(), frsp, frspOrig, BSON( "a" << 1 ), BSON( "b" << 1 ), true, &e );
                ASSERT_EQUALS( 1, s.nPlans() );
            }
        };

        class HintName : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), false, "a_1" );
                Helpers::ensureIndex( ns(), BSON( "b" << 1 ), false, "b_1" );
                BSONObj b = BSON( "hint" << "a_1" );
                BSONElement e = b.firstElement();
                auto_ptr< FieldRangeSetPair > frsp( new FieldRangeSetPair( ns(), BSON( "a" << 1 ) ) );
                auto_ptr< FieldRangeSetPair > frspOrig( new FieldRangeSetPair( *frsp ) );
                QueryPlanSet s( ns(), frsp, frspOrig, BSON( "a" << 1 ), BSON( "b" << 1 ), true, &e );
                ASSERT_EQUALS( 1, s.nPlans() );
            }
        };

        class NaturalHint : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), false, "a_1" );
                Helpers::ensureIndex( ns(), BSON( "b" << 1 ), false, "b_1" );
                BSONObj b = BSON( "hint" << BSON( "$natural" << 1 ) );
                BSONElement e = b.firstElement();
                auto_ptr< FieldRangeSetPair > frsp( new FieldRangeSetPair( ns(), BSON( "a" << 1 ) ) );
                auto_ptr< FieldRangeSetPair > frspOrig( new FieldRangeSetPair( *frsp ) );
                QueryPlanSet s( ns(), frsp, frspOrig, BSON( "a" << 1 ), BSON( "b" << 1 ), true, &e );
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
                QueryPlanSet s( ns(), frsp, frspOrig, BSON( "a" << 1 ), BSON( "$natural" << 1 ) );
                ASSERT_EQUALS( 1, s.nPlans() );
            }
        };

        class BadHint : public Base {
        public:
            void run() {
                BSONObj b = BSON( "hint" << "a_1" );
                BSONElement e = b.firstElement();
                auto_ptr< FieldRangeSetPair > frsp( new FieldRangeSetPair( ns(), BSON( "a" << 1 ) ) );
                auto_ptr< FieldRangeSetPair > frspOrig( new FieldRangeSetPair( *frsp ) );
                ASSERT_THROWS( QueryPlanSet s( ns(), frsp, frspOrig, BSON( "a" << 1 ), BSON( "b" << 1 ), true, &e ),
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
                QueryPlanSet s( ns(), frsp, frspOrig, BSON( "a" << 1 << "c" << 2 ), BSONObj() );
                ASSERT_EQUALS( 2, s.nPlans() );
            }
        };

        class SingleException : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), false, "a_1" );
                Helpers::ensureIndex( ns(), BSON( "b" << 1 ), false, "b_1" );
                auto_ptr< FieldRangeSetPair > frsp( new FieldRangeSetPair( ns(), BSON( "a" << 4 ) ) );
                auto_ptr< FieldRangeSetPair > frspOrig( new FieldRangeSetPair( *frsp ) );
                QueryPlanSet s( ns(), frsp, frspOrig, BSON( "a" << 4 ), BSON( "b" << 1 ) );
                ASSERT_EQUALS( 3, s.nPlans() );
                bool threw = false;
                auto_ptr< TestOp > t( new TestOp( true, threw ) );
                boost::shared_ptr< TestOp > done = s.runOp( *t );
                ASSERT( threw );
                ASSERT( done->complete() );
                ASSERT( done->exception().empty() );
                ASSERT( !done->error() );
            }
        private:
            class TestOp : public QueryOp {
            public:
                TestOp( bool iThrow, bool &threw ) : iThrow_( iThrow ), threw_( threw ), i_(), youThrow_( false ) {}
                virtual void _init() {}
                virtual void next() {
                    if ( iThrow_ )
                        threw_ = true;
                    massert( 10408 ,  "throw", !iThrow_ );
                    if ( ++i_ > 10 )
                        setComplete();
                }
                virtual QueryOp *_createChild() const {
                    QueryOp *op = new TestOp( youThrow_, threw_ );
                    youThrow_ = !youThrow_;
                    return op;
                }
                virtual bool mayRecordPlan() const { return true; }
                virtual long long nscanned() { return 0; }
            private:
                bool iThrow_;
                bool &threw_;
                int i_;
                mutable bool youThrow_;
            };
        };

        class AllException : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), false, "a_1" );
                Helpers::ensureIndex( ns(), BSON( "b" << 1 ), false, "b_1" );
                auto_ptr< FieldRangeSetPair > frsp( new FieldRangeSetPair( ns(), BSON( "a" << 4 ) ) );
                auto_ptr< FieldRangeSetPair > frspOrig( new FieldRangeSetPair( *frsp ) );
                QueryPlanSet s( ns(), frsp, frspOrig, BSON( "a" << 4 ), BSON( "b" << 1 ) );
                ASSERT_EQUALS( 3, s.nPlans() );
                auto_ptr< TestOp > t( new TestOp() );
                boost::shared_ptr< TestOp > done = s.runOp( *t );
                ASSERT( !done->complete() );
                ASSERT_EQUALS( "throw", done->exception().msg );
                ASSERT( done->error() );
            }
        private:
            class TestOp : public QueryOp {
            public:
                virtual void _init() {}
                virtual void next() {
                    massert( 10409 ,  "throw", false );
                }
                virtual QueryOp *_createChild() const {
                    return new TestOp();
                }
                virtual bool mayRecordPlan() const { return true; }
                virtual long long nscanned() { return 0; }
            };
        };

        class SaveGoodIndex : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), false, "a_1" );
                Helpers::ensureIndex( ns(), BSON( "b" << 1 ), false, "b_1" );
                // No best plan - all must be tried.
                nPlans( 3 );
                runQuery();
                // Best plan selected by query.
                nPlans( 1 );
                nPlans( 1 );
                Helpers::ensureIndex( ns(), BSON( "c" << 1 ), false, "c_1" );
                // Best plan cleared when new index added.
                nPlans( 3 );
                runQuery();
                // Best plan selected by query.
                nPlans( 1 );

                {
                    DBDirectClient client;
                    for( int i = 0; i < 334; ++i ) {
                        client.insert( ns(), BSON( "i" << i ) );
                        client.update( ns(), QUERY( "i" << i ), BSON( "i" << i + 1 ) );
                        client.remove( ns(), BSON( "i" << i + 1 ) );
                    }
                }
                // Best plan cleared by ~1000 writes.
                nPlans( 3 );

                auto_ptr< FieldRangeSetPair > frsp( new FieldRangeSetPair( ns(), BSON( "a" << 4 ) ) );
                auto_ptr< FieldRangeSetPair > frspOrig( new FieldRangeSetPair( *frsp ) );
                QueryPlanSet s( ns(), frsp, frspOrig, BSON( "a" << 4 ), BSON( "b" << 1 ) );
                NoRecordTestOp original;
                s.runOp( original );
                // NoRecordTestOp doesn't record a best plan (test cases where mayRecordPlan() is false).
                nPlans( 3 );

                BSONObj hint = fromjson( "{hint:{$natural:1}}" );
                BSONElement hintElt = hint.firstElement();
                auto_ptr< FieldRangeSetPair > frsp2( new FieldRangeSetPair( ns(), BSON( "a" << 4 ) ) );
                auto_ptr< FieldRangeSetPair > frspOrig2( new FieldRangeSetPair( *frsp2 ) );
                QueryPlanSet s2( ns(), frsp2, frspOrig2, BSON( "a" << 4 ), BSON( "b" << 1 ), true, &hintElt );
                TestOp newOriginal;
                s2.runOp( newOriginal );
                // No plan recorded when a hint is used.
                nPlans( 3 );

                auto_ptr< FieldRangeSetPair > frsp3( new FieldRangeSetPair( ns(), BSON( "a" << 4 ), true ) );
                auto_ptr< FieldRangeSetPair > frspOrig3( new FieldRangeSetPair( *frsp3 ) );
                QueryPlanSet s3( ns(), frsp3, frspOrig3, BSON( "a" << 4 ), BSON( "b" << 1 << "c" << 1 ) );
                TestOp newerOriginal;
                s3.runOp( newerOriginal );
                // Plan recorded was for a different query pattern (different sort spec).
                nPlans( 3 );

                // Best plan still selected by query after all these other tests.
                runQuery();
                nPlans( 1 );
            }
        private:
            void nPlans( int n ) {
                auto_ptr< FieldRangeSetPair > frsp( new FieldRangeSetPair( ns(), BSON( "a" << 4 ) ) );
                auto_ptr< FieldRangeSetPair > frspOrig( new FieldRangeSetPair( *frsp ) );
                QueryPlanSet s( ns(), frsp, frspOrig, BSON( "a" << 4 ), BSON( "b" << 1 ) );
                ASSERT_EQUALS( n, s.nPlans() );
            }
            void runQuery() {
                auto_ptr< FieldRangeSetPair > frsp( new FieldRangeSetPair( ns(), BSON( "a" << 4 ) ) );
                auto_ptr< FieldRangeSetPair > frspOrig( new FieldRangeSetPair( *frsp ) );
                QueryPlanSet s( ns(), frsp, frspOrig, BSON( "a" << 4 ), BSON( "b" << 1 ) );
                TestOp original;
                s.runOp( original );
            }
            class TestOp : public QueryOp {
            public:
                virtual void _init() {}
                virtual void next() {
                    setComplete();
                }
                virtual QueryOp *_createChild() const {
                    return new TestOp();
                }
                virtual bool mayRecordPlan() const { return true; }
                virtual long long nscanned() { return 0; }
            };
            class NoRecordTestOp : public TestOp {
                virtual bool mayRecordPlan() const { return false; }
                virtual QueryOp *_createChild() const { return new NoRecordTestOp(); }
            };
        };

        class TryAllPlansOnErr : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), false, "a_1" );

                auto_ptr< FieldRangeSetPair > frsp( new FieldRangeSetPair( ns(), BSON( "a" << 4 ) ) );
                auto_ptr< FieldRangeSetPair > frspOrig( new FieldRangeSetPair( *frsp ) );
                QueryPlanSet s( ns(), frsp, frspOrig, BSON( "a" << 4 ), BSON( "b" << 1 ) );
                ScanOnlyTestOp op;
                s.runOp( op );
                pair< BSONObj, long long > best = QueryUtilIndexed::bestIndexForPatterns( s.frsp(), BSON( "b" << 1 ) );
                ASSERT( fromjson( "{$natural:1}" ).woCompare( best.first ) == 0 );
                ASSERT_EQUALS( 1, best.second );

                auto_ptr< FieldRangeSetPair > frsp2( new FieldRangeSetPair( ns(), BSON( "a" << 4 ) ) );
                auto_ptr< FieldRangeSetPair > frspOrig2( new FieldRangeSetPair( *frsp2 ) );
                QueryPlanSet s2( ns(), frsp2, frspOrig2, BSON( "a" << 4 ), BSON( "b" << 1 ) );
                TestOp op2;
                ASSERT( s2.runOp( op2 )->complete() );
            }
        private:
            class TestOp : public QueryOp {
            public:
                TestOp() {}
                virtual void _init() {}
                virtual void next() {
                    if ( qp().indexKey().firstElementFieldName() == string( "$natural" ) )
                        massert( 10410 ,  "throw", false );
                    setComplete();
                }
                virtual QueryOp *_createChild() const {
                    return new TestOp();
                }
                virtual bool mayRecordPlan() const { return true; }
                virtual long long nscanned() { return 1; }
            };
            class ScanOnlyTestOp : public TestOp {
                virtual void next() {
                    if ( qp().indexKey().firstElement().fieldName() == string( "$natural" ) )
                        setComplete();
                    massert( 10411 ,  "throw", false );
                }
                virtual QueryOp *_createChild() const {
                    return new ScanOnlyTestOp();
                }
            };
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
                ASSERT( BSON( "a" << 1 ).woCompare( NamespaceDetailsTransient::get_inlock( ns() ).indexForPattern( FieldRangeSet( ns(), delSpec, true ).pattern() ) ) == 0 );
                ASSERT_EQUALS( 1, NamespaceDetailsTransient::get_inlock( ns() ).nScannedForPattern( FieldRangeSet( ns(), delSpec, true ).pattern() ) );
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

        class TryOtherPlansBeforeFinish : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), false, "a_1" );
                for( int i = 0; i < 100; ++i ) {
                    for( int j = 0; j < 2; ++j ) {
                        BSONObj temp = BSON( "a" << 100 - i - 1 << "b" << i );
                        theDataFileMgr.insertWithObjMod( ns(), temp );
                    }
                }
                Message m;
                // Need to return at least 2 records to cause plan to be recorded.
                assembleRequest( ns(), QUERY( "b" << 0 << "a" << GTE << 0 ).obj, 2, 0, 0, 0, m );
                stringstream ss;
                {
                    DbMessage d(m);
                    QueryMessage q(d);
                    runQuery( m, q);
                }
                ASSERT( BSON( "$natural" << 1 ).woCompare( NamespaceDetailsTransient::get_inlock( ns() ).indexForPattern( FieldRangeSet( ns(), BSON( "b" << 0 << "a" << GTE << 0 ), true ).pattern() ) ) == 0 );

                Message m2;
                assembleRequest( ns(), QUERY( "b" << 99 << "a" << GTE << 0 ).obj, 2, 0, 0, 0, m2 );
                {
                    DbMessage d(m2);
                    QueryMessage q(d);
                    runQuery( m2, q);
                }
                ASSERT( BSON( "a" << 1 ).woCompare( NamespaceDetailsTransient::get_inlock( ns() ).indexForPattern( FieldRangeSet( ns(), BSON( "b" << 0 << "a" << GTE << 0 ), true ).pattern() ) ) == 0 );
                ASSERT_EQUALS( 3, NamespaceDetailsTransient::get_inlock( ns() ).nScannedForPattern( FieldRangeSet( ns(), BSON( "b" << 0 << "a" << GTE << 0 ), true ).pattern() ) );
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
                BSONElement hintElt = hint.firstElement();
                auto_ptr< FieldRangeSetPair > frsp( new FieldRangeSetPair( ns(), fromjson( "{a:{$in:[2,3,6,9,11]}}" ) ) );
                auto_ptr< FieldRangeSetPair > frspOrig( new FieldRangeSetPair( *frsp ) );
                QueryPlanSet s( ns(), frsp, frspOrig, fromjson( "{a:{$in:[2,3,6,9,11]}}" ), BSONObj(), true, &hintElt );
                QueryPlan qp( nsd(), 1, s.frsp(), s.originalFrsp(), fromjson( "{a:{$in:[2,3,6,9,11]}}" ), BSONObj() );
                boost::shared_ptr<Cursor> c = qp.newCursor();
                double expected[] = { 2, 3, 6, 9 };
                for( int i = 0; i < 4; ++i, c->advance() ) {
                    ASSERT_EQUALS( expected[ i ], c->current().getField( "a" ).number() );
                }
                ASSERT( !c->ok() );

                // now check reverse
                {
                    auto_ptr< FieldRangeSetPair > frsp( new FieldRangeSetPair( ns(), fromjson( "{a:{$in:[2,3,6,9,11]}}" ) ) );
                    auto_ptr< FieldRangeSetPair > frspOrig( new FieldRangeSetPair( *frsp ) );
                    QueryPlanSet s( ns(), frsp, frspOrig, fromjson( "{a:{$in:[2,3,6,9,11]}}" ), BSON( "a" << -1 ), true, &hintElt );
                    QueryPlan qp( nsd(), 1, s.frsp(), s.originalFrsp(), fromjson( "{a:{$in:[2,3,6,9,11]}}" ), BSON( "a" << -1 ) );
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
                BSONObj hint = fromjson( "{$hint:{a:1,b:1}}" );
                auto_ptr< FieldRangeSetPair > frsp( new FieldRangeSetPair( ns(), fromjson( "{a:5,b:{$in:[2,3,6,9,11]}}" ) ) );
                QueryPlan qp( nsd(), 1, *frsp, frsp.get(), fromjson( "{a:5,b:{$in:[2,3,6,9,11]}}" ), BSONObj() );
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
                BSONObj hint = fromjson( "{$hint:{a:1,b:1}}" );
                auto_ptr< FieldRangeSetPair > frsp( new FieldRangeSetPair( ns(), fromjson( "{a:{$gte:5},b:{$in:[2,3,6,9,11]}}" ) ) );
                QueryPlan qp( nsd(), 1, *frsp, frsp.get(), fromjson( "{a:{$gte:5},b:{$in:[2,3,6,9,11]}}" ), BSONObj() );
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
    private:
        dblock lk_;
        Client::Context _ctx;
    };

    class BestGuess : public Base {
    public:
        void run() {
            Helpers::ensureIndex( ns(), BSON( "a" << 1 ), false, "a_1" );
            Helpers::ensureIndex( ns(), BSON( "b" << 1 ), false, "b_1" );
            BSONObj temp = BSON( "a" << 1 );
            theDataFileMgr.insertWithObjMod( ns(), temp );
            temp = BSON( "b" << 1 );
            theDataFileMgr.insertWithObjMod( ns(), temp );

            boost::shared_ptr< Cursor > c = bestGuessCursor( ns(), BSON( "b" << 1 ), BSON( "a" << 1 ) );
            ASSERT_EQUALS( string( "a" ), c->indexKeyPattern().firstElement().fieldName() );
            c = bestGuessCursor( ns(), BSON( "a" << 1 ), BSON( "b" << 1 ) );
            ASSERT_EQUALS( string( "b" ), c->indexKeyPattern().firstElementFieldName() );
            boost::shared_ptr< MultiCursor > m = dynamic_pointer_cast< MultiCursor >( bestGuessCursor( ns(), fromjson( "{b:1,$or:[{z:1}]}" ), BSON( "a" << 1 ) ) );
            ASSERT_EQUALS( string( "a" ), m->sub_c()->indexKeyPattern().firstElement().fieldName() );
            m = dynamic_pointer_cast< MultiCursor >( bestGuessCursor( ns(), fromjson( "{a:1,$or:[{y:1}]}" ), BSON( "b" << 1 ) ) );
            ASSERT_EQUALS( string( "b" ), m->sub_c()->indexKeyPattern().firstElementFieldName() );

            FieldRangeSet frs( "ns", BSON( "a" << 1 ), true );
            {
                SimpleMutex::scoped_lock lk(NamespaceDetailsTransient::_qcMutex);
                NamespaceDetailsTransient::get_inlock( ns() ).registerIndexForPattern( frs.pattern( BSON( "b" << 1 ) ), BSON( "a" << 1 ), 0 );
            }
            m = dynamic_pointer_cast< MultiCursor >( bestGuessCursor( ns(), fromjson( "{a:1,$or:[{y:1}]}" ), BSON( "b" << 1 ) ) );
            ASSERT_EQUALS( string( "b" ), m->sub_c()->indexKeyPattern().firstElement().fieldName() );
        }
    };
    
    class BestGuessOrSortAssertion : public Base {
    public:
        void run() {
            ASSERT_THROWS( bestGuessCursor( ns(), BSON( "$or" << BSON_ARRAY( BSON( "b" << 1 ) ) ), BSON( "a" << 1 ) ), MsgAssertionException );
        }
    };
    
    class All : public Suite {
    public:
        All() : Suite( "queryoptimizer" ) {}

        void setupTests() {
            __forceLinkGeoPlugin();
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
            add<QueryPlanSetTests::SingleException>();
            add<QueryPlanSetTests::AllException>();
            add<QueryPlanSetTests::SaveGoodIndex>();
            add<QueryPlanSetTests::TryAllPlansOnErr>();
            add<QueryPlanSetTests::FindOne>();
            add<QueryPlanSetTests::Delete>();
            add<QueryPlanSetTests::DeleteOneScan>();
            add<QueryPlanSetTests::DeleteOneIndex>();
            add<QueryPlanSetTests::TryOtherPlansBeforeFinish>();
            add<QueryPlanSetTests::InQueryIntervals>();
            add<QueryPlanSetTests::EqualityThenIn>();
            add<QueryPlanSetTests::NotEqualityThenIn>();
            add<QueryPlanSetTests::ExcludeSpecialPlanWhenBtreePlan>();
            add<QueryPlanSetTests::ExcludeUnindexedPlanWhenSpecialPlan>();
            add<BestGuess>();
            add<BestGuessOrSortAssertion>();
        }
    } myall;

} // namespace QueryOptimizerTests

