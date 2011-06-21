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
#include "../db/querypattern.h"
#include "../db/instance.h"
#include "../db/query.h"
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
    shared_ptr<Cursor> newQueryOptimizerCursor( const char *ns, const BSONObj &query, const BSONObj &order = BSONObj() );
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
#define FRSP2(x) ( FieldRangeSetPair_GLOBAL2.reset( new FieldRangeSetPair( ns(), x ) ), *FieldRangeSetPair_GLOBAL2 )

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
                QueryPlanSet s( ns(), frsp, frspOrig, BSON( "a" << 1 ), BSON( "b" << 1 ), &e );
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
                QueryPlanSet s( ns(), frsp, frspOrig, BSON( "a" << 1 ), BSON( "b" << 1 ), &e );
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
                QueryPlanSet s( ns(), frsp, frspOrig, BSON( "a" << 1 ), BSON( "b" << 1 ), &e );
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
                ASSERT_EXCEPTION( QueryPlanSet s( ns(), frsp, frspOrig, BSON( "a" << 1 ), BSON( "b" << 1 ), &e ),
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
                nPlans( 3 );
                runQuery();
                nPlans( 1 );
                nPlans( 1 );
                Helpers::ensureIndex( ns(), BSON( "c" << 1 ), false, "c_1" );
                nPlans( 3 );
                runQuery();
                nPlans( 1 );

                {
                    DBDirectClient client;
                    for( int i = 0; i < 34; ++i ) {
                        client.insert( ns(), BSON( "i" << i ) );
                        client.update( ns(), QUERY( "i" << i ), BSON( "i" << i + 1 ) );
                        client.remove( ns(), BSON( "i" << i + 1 ) );
                    }
                }
                nPlans( 3 );

                auto_ptr< FieldRangeSetPair > frsp( new FieldRangeSetPair( ns(), BSON( "a" << 4 ) ) );
                auto_ptr< FieldRangeSetPair > frspOrig( new FieldRangeSetPair( *frsp ) );
                QueryPlanSet s( ns(), frsp, frspOrig, BSON( "a" << 4 ), BSON( "b" << 1 ) );
                NoRecordTestOp original;
                s.runOp( original );
                nPlans( 3 );

                BSONObj hint = fromjson( "{hint:{$natural:1}}" );
                BSONElement hintElt = hint.firstElement();
                auto_ptr< FieldRangeSetPair > frsp2( new FieldRangeSetPair( ns(), BSON( "a" << 4 ) ) );
                auto_ptr< FieldRangeSetPair > frspOrig2( new FieldRangeSetPair( *frsp2 ) );
                QueryPlanSet s2( ns(), frsp2, frspOrig2, BSON( "a" << 4 ), BSON( "b" << 1 ), &hintElt );
                TestOp newOriginal;
                s2.runOp( newOriginal );
                nPlans( 3 );

                auto_ptr< FieldRangeSetPair > frsp3( new FieldRangeSetPair( ns(), BSON( "a" << 4 ), true ) );
                auto_ptr< FieldRangeSetPair > frspOrig3( new FieldRangeSetPair( *frsp3 ) );
                QueryPlanSet s3( ns(), frsp3, frspOrig3, BSON( "a" << 4 ), BSON( "b" << 1 << "c" << 1 ) );
                TestOp newerOriginal;
                s3.runOp( newerOriginal );
                nPlans( 3 );

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
                ASSERT_EXCEPTION( Helpers::findOne( ns(), BSON( "a" << 1 ), result, true ), AssertionException );
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
                deleteObjects( ns(), BSON( "a" << 1 ), false );
                ASSERT( BSON( "a" << 1 ).woCompare( NamespaceDetailsTransient::get_inlock( ns() ).indexForPattern( FieldRangeSet( ns(), BSON( "a" << 1 ), true ).pattern() ) ) == 0 );
                ASSERT_EQUALS( 1, NamespaceDetailsTransient::get_inlock( ns() ).nScannedForPattern( FieldRangeSet( ns(), BSON( "a" << 1 ), true ).pattern() ) );
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
                QueryPlanSet s( ns(), frsp, frspOrig, fromjson( "{a:{$in:[2,3,6,9,11]}}" ), BSONObj(), &hintElt );
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
                    QueryPlanSet s( ns(), frsp, frspOrig, fromjson( "{a:{$in:[2,3,6,9,11]}}" ), BSON( "a" << -1 ), &hintElt );
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
                QueryPlan qp( nsd(), 1, *frsp, *frsp, fromjson( "{a:5,b:{$in:[2,3,6,9,11]}}" ), BSONObj() );
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
                QueryPlan qp( nsd(), 1, *frsp, *frsp, fromjson( "{a:{$gte:5},b:{$in:[2,3,6,9,11]}}" ), BSONObj() );
                boost::shared_ptr<Cursor> c = qp.newCursor();
                int matches[] = { 2, 3, 6, 9 };
                for( int i = 0; i < 4; ++i, c->advance() ) {
                    ASSERT_EQUALS( matches[ i ], c->current().getField( "b" ).number() );
                }
                ASSERT( !c->ok() );
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
    
    namespace QueryOptimizerCursorTests {
        
        using boost::shared_ptr;

        class Base {
        public:
            Base() {
                dblock lk;
                Client::Context ctx( ns() );
                string err;
                userCreateNS( ns(), BSONObj(), err, false );
                dropCollection( ns() );
            }
            ~Base() {
             	cc().curop()->reset();
            }
        protected:
            DBDirectClient _cli;
            static const char *ns() { return "unittests.QueryOptimizerTests"; }
            void setQueryOptimizerCursor( const BSONObj &query, const BSONObj &order = BSONObj() ) {
             	_c = newQueryOptimizerCursor( ns(), query, order );
                if ( ok() && !mayReturnCurrent() ) {
                 	advance();
                }
            }
            bool ok() const { return _c->ok(); }
            /** Handles matching and deduping. */
            bool advance() {
             	while( _c->advance() && !mayReturnCurrent() );
                return ok();
            }
            int itcount() {
                int ret = 0;
             	while( ok() ) {
                    ++ret;
                    advance();
                }
                return ret;
            }
            BSONObj current() const { return _c->current(); }
            bool mayReturnCurrent() {
             	return _c->matcher()->matchesCurrent( _c.get() ) && !_c->getsetdup( _c->currLoc() );
            }
            bool prepareToYield() const { return _c->prepareToYield(); }
            void recoverFromYield() {
                _c->recoverFromYield();
                if ( ok() && !mayReturnCurrent() ) {
                 	advance();   
                }
            }
            shared_ptr<Cursor> c() { return _c; }
            long long nscanned() const { return _c->nscanned(); }
        private:
            shared_ptr<Cursor> _c;
        };
        
        /** No results for empty collection. */
        class Empty : public Base {
        public:
            void run() {
                dblock lk;
                Client::Context ctx( ns() );
                shared_ptr<Cursor> c = newQueryOptimizerCursor( ns(), BSONObj() );
                ASSERT( !c->ok() );
                ASSERT_EXCEPTION( c->_current(), AssertionException );
                ASSERT_EXCEPTION( c->current(), AssertionException );
                ASSERT( c->currLoc().isNull() );
                ASSERT( !c->advance() );
                ASSERT_EXCEPTION( c->currKey(), AssertionException );
                ASSERT_EXCEPTION( c->getsetdup( DiskLoc() ), AssertionException );
                ASSERT_EXCEPTION( c->isMultiKey(), AssertionException );
                ASSERT_EXCEPTION( c->matcher(), AssertionException );
            }
        };
        
        /** Simple table scan. */
        class Unindexed : public Base {
        public:
            void run() {
                _cli.insert( ns(), BSON( "_id" << 1 ) );
                _cli.insert( ns(), BSON( "_id" << 2 ) );

                dblock lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSONObj() );
                ASSERT_EQUALS( 2, itcount() );
            }
        };
        
        /** Basic test with two indexes and deduping requirement. */
        class Basic : public Base {
        public:
            void run() {
                _cli.insert( ns(), BSON( "_id" << 1 << "a" << 2 ) );
                _cli.insert( ns(), BSON( "_id" << 2 << "a" << 1 ) );
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                
                dblock lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "_id" << GT << 0 << "a" << GT << 0 ) );
                ASSERT( ok() );
                ASSERT_EQUALS( BSON( "_id" << 1 << "a" << 2 ), current() );
                ASSERT( advance() );
                ASSERT_EQUALS( BSON( "_id" << 2 << "a" << 1 ), current() );
                ASSERT( !advance() );
                ASSERT( !ok() );
            }
        };
        
        class NoMatch : public Base {
        public:
            void run() {
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                
                dblock lk;
                Client::Context ctx( ns() );
				setQueryOptimizerCursor( BSON( "_id" << GT << 5 << LT << 4 << "a" << GT << 0 ) );
                ASSERT( !ok() );
            }            
        };
        
        /** Order of results indicates that interleaving is occurring. */
        class Interleaved : public Base {
        public:
            void run() {
                _cli.insert( ns(), BSON( "_id" << 1 << "a" << 2 ) );
                _cli.insert( ns(), BSON( "_id" << 3 << "a" << 1 ) );
                _cli.insert( ns(), BSON( "_id" << 2 << "a" << 2 ) );
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                
                dblock lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "_id" << GT << 0 << "a" << GT << 0 ) );
                ASSERT( ok() );
                ASSERT_EQUALS( BSON( "_id" << 1 << "a" << 2 ), current() );
                ASSERT( advance() );
                ASSERT_EQUALS( BSON( "_id" << 3 << "a" << 1 ), current() );
                ASSERT( advance() );
                ASSERT_EQUALS( BSON( "_id" << 2 << "a" << 2 ), current() );
                ASSERT( !advance() );
                ASSERT( !ok() );
            }
        };
        
        /** Some values on each index do not match. */
        class NotMatch : public Base {
        public:
            void run() {
                _cli.insert( ns(), BSON( "_id" << 0 << "a" << 10 ) );
                _cli.insert( ns(), BSON( "_id" << 10 << "a" << 0 ) );
                _cli.insert( ns(), BSON( "_id" << 11 << "a" << 12 ) );
                _cli.insert( ns(), BSON( "_id" << 12 << "a" << 11 ) );
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                
                dblock lk;
                Client::Context ctx( ns() );
				setQueryOptimizerCursor( BSON( "_id" << GT << 5 << "a" << GT << 5 ) );
                ASSERT( ok() );
                ASSERT_EQUALS( BSON( "_id" << 11 << "a" << 12 ), current() );
                ASSERT( advance() );
                ASSERT_EQUALS( BSON( "_id" << 12 << "a" << 11 ), current() );
                ASSERT( !advance() );
                ASSERT( !ok() );
            }            
        };
        
        /** After the first 101 matches for a plan, we stop interleaving the plans. */
        class StopInterleaving : public Base {
        public:
            void run() {
             	for( int i = 0; i < 101; ++i ) {
                 	_cli.insert( ns(), BSON( "_id" << i << "a" << i ) );   
                }
             	for( int i = 101; i < 200; ++i ) {
                 	_cli.insert( ns(), BSON( "_id" << i << "a" << (301-i) ) );   
                }
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                
                dblock lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "_id" << GT << -1 << "a" << GT << -1 ) );
                for( int i = 0; i < 200; ++i ) {
                    ASSERT( ok() );
                    ASSERT_EQUALS( i, current().getIntField( "_id" ) );
                    advance();
                }
                ASSERT( !advance() );
                ASSERT( !ok() );                
            }
        };
        
        /** Test correct deduping with the takeover cursor. */
        class TakeoverWithDup : public Base {
        public:
            void run() {
             	for( int i = 0; i < 101; ++i ) {
                 	_cli.insert( ns(), BSON( "_id" << i << "a" << i ) );   
                }
                _cli.insert( ns(), BSON( "_id" << 500 << "a" << BSON_ARRAY( 0 << 300 ) ) );
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                dblock lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "_id" << GT << -1 << "a" << GT << -1 ) );
                ASSERT_EQUALS( 102, itcount() );
            }
        };
        
        /** Test usage of matcher with takeover cursor. */
        class TakeoverWithNonMatches : public Base {
        public:
            void run() {
             	for( int i = 0; i < 101; ++i ) {
                 	_cli.insert( ns(), BSON( "_id" << i << "a" << i ) );   
                }
                _cli.insert( ns(), BSON( "_id" << 101 << "a" << 600 ) );
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                dblock lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "_id" << GT << -1 << "a" << LT << 500 ) );
                ASSERT_EQUALS( 101, itcount() );
            }
        };
        
        /** Check deduping of dups within just the takeover cursor. */
        class TakeoverWithTakeoverDup : public Base {
        public:
            void run() {
             	for( int i = 0; i < 101; ++i ) {
                 	_cli.insert( ns(), BSON( "_id" << i*2 << "a" << 0 ) );
                 	_cli.insert( ns(), BSON( "_id" << i*2+1 << "a" << 1 ) );
                }
                _cli.insert( ns(), BSON( "_id" << 202 << "a" << BSON_ARRAY( 2 << 3 ) ) );
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                dblock lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "_id" << GT << -1 << "a" << GT << 0) );
                ASSERT_EQUALS( 102, itcount() );
            }
        };
        
        /** Basic test with $or query. */
        class BasicOr : public Base {
        public:
            void run() {
                _cli.insert( ns(), BSON( "_id" << 0 << "a" << 0 ) );
                _cli.insert( ns(), BSON( "_id" << 1 << "a" << 1 ) );
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                dblock lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "$or" << BSON_ARRAY( BSON( "_id" << 0 ) << BSON( "a" << 1 ) ) ) );
                ASSERT_EQUALS( BSON( "_id" << 0 << "a" << 0 ), current() );
                ASSERT( advance() );
                ASSERT_EQUALS( BSON( "_id" << 1 << "a" << 1 ), current() );
                ASSERT( !advance() );
            }
        };

        /** $or first clause empty. */
        class OrFirstClauseEmpty : public Base {
        public:
            void run() {
                _cli.insert( ns(), BSON( "_id" << 0 << "a" << 1 ) );
                _cli.insert( ns(), BSON( "_id" << 1 << "a" << 1 ) );
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                dblock lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "$or" << BSON_ARRAY( BSON( "_id" << -1 ) << BSON( "a" << 1 ) ) ) );
                ASSERT_EQUALS( BSON( "_id" << 0 << "a" << 1 ), current() );
                ASSERT( advance() );
                ASSERT_EQUALS( BSON( "_id" << 1 << "a" << 1 ), current() );
                ASSERT( !advance() );
            }
        };        

        /** $or second clause empty. */
        class OrSecondClauseEmpty : public Base {
        public:
            void run() {
                _cli.insert( ns(), BSON( "_id" << 0 << "a" << 1 ) );
                _cli.insert( ns(), BSON( "_id" << 1 << "a" << 1 ) );
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                dblock lk;
                Client::Context ctx( ns() );
				setQueryOptimizerCursor( BSON( "$or" << BSON_ARRAY( BSON( "_id" << 0 ) << BSON( "_id" << -1 ) << BSON( "a" << 1 ) ) ) );
                ASSERT_EQUALS( BSON( "_id" << 0 << "a" << 1 ), current() );
                ASSERT( advance() );
                ASSERT_EQUALS( BSON( "_id" << 1 << "a" << 1 ), current() );
                ASSERT( !advance() );
            }
        };
        
        /** $or multiple clauses empty empty. */
        class OrMultipleClausesEmpty : public Base {
        public:
            void run() {
                _cli.insert( ns(), BSON( "_id" << 0 << "a" << 1 ) );
                _cli.insert( ns(), BSON( "_id" << 1 << "a" << 1 ) );
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                dblock lk;
                Client::Context ctx( ns() );
				setQueryOptimizerCursor( BSON( "$or" << BSON_ARRAY( BSON( "_id" << 2 ) << BSON( "_id" << 4 ) << BSON( "_id" << 0 ) << BSON( "_id" << -1 ) << BSON( "_id" << 6 ) << BSON( "a" << 1 ) << BSON( "_id" << 9 ) ) ) );
                ASSERT_EQUALS( BSON( "_id" << 0 << "a" << 1 ), current() );
                ASSERT( advance() );
                ASSERT_EQUALS( BSON( "_id" << 1 << "a" << 1 ), current() );
                ASSERT( !advance() );
            }
        };
        
        /** Check that takeover occurs at proper match count with $or clauses */
    	class TakeoverCountOr : public Base {
        public:
            void run() {
             	for( int i = 0; i < 60; ++i ) {
                 	_cli.insert( ns(), BSON( "_id" << i << "a" << 0 ) );   
                }
             	for( int i = 60; i < 120; ++i ) {
                 	_cli.insert( ns(), BSON( "_id" << i << "a" << 1 ) );
                }
             	for( int i = 120; i < 150; ++i ) {
                 	_cli.insert( ns(), BSON( "_id" << i << "a" << (200-i) ) );
                }
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                dblock lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "$or" << BSON_ARRAY( BSON( "a" << 0 ) << BSON( "a" << 1 ) << BSON( "_id" << GTE << 120 << "a" << GT << 1 ) ) ) );
                for( int i = 0; i < 120; ++i ) {
                 	ASSERT( ok() );
                    advance();
                }
                // Expect to be scanning on _id index only.
                for( int i = 120; i < 150; ++i ) {
                 	ASSERT_EQUALS( i, current().getIntField( "_id" ) );
                    advance();
                }
                ASSERT( !ok() );
            }
        };
        
        /** Takeover just at end of clause. */
        class TakeoverEndOfOrClause : public Base {
        public:
            void run() {
                for( int i = 0; i < 102; ++i ) {
                 	_cli.insert( ns(), BSON( "_id" << i ) );   
                }
                dblock lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "$or" << BSON_ARRAY( BSON( "_id" << LT << 101 ) << BSON( "_id" << 101 ) ) ) );
                for( int i = 0; i < 102; ++i ) {
                 	ASSERT_EQUALS( i, current().getIntField( "_id" ) );
                    advance();
                }
                ASSERT( !ok() );
            }
        };

        class TakeoverBeforeEndOfOrClause : public Base {
        public:
            void run() {
                for( int i = 0; i < 101; ++i ) {
                 	_cli.insert( ns(), BSON( "_id" << i ) );   
                }
                dblock lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "$or" << BSON_ARRAY( BSON( "_id" << LT << 100 ) << BSON( "_id" << 100 ) ) ) );
                for( int i = 0; i < 101; ++i ) {
                 	ASSERT_EQUALS( i, current().getIntField( "_id" ) );
                    advance();
                }
                ASSERT( !ok() );
            }
        };

        class TakeoverAfterEndOfOrClause : public Base {
        public:
            void run() {
                for( int i = 0; i < 103; ++i ) {
                 	_cli.insert( ns(), BSON( "_id" << i ) );   
                }
                dblock lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "$or" << BSON_ARRAY( BSON( "_id" << LT << 102 ) << BSON( "_id" << 102 ) ) ) );
                for( int i = 0; i < 103; ++i ) {
                 	ASSERT_EQUALS( i, current().getIntField( "_id" ) );
                    advance();
                }
                ASSERT( !ok() );
            }
        };
        
        /** Test matching and deduping done manually by cursor client. */
        class ManualMatchingDeduping : public Base {
        public:
            void run() {
                _cli.insert( ns(), BSON( "_id" << 0 << "a" << 10 ) );
                _cli.insert( ns(), BSON( "_id" << 10 << "a" << 0 ) ); 
                _cli.insert( ns(), BSON( "_id" << 11 << "a" << 12 ) );
                _cli.insert( ns(), BSON( "_id" << 12 << "a" << 11 ) );
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                
                dblock lk;
                Client::Context ctx( ns() );
                shared_ptr< Cursor > c = newQueryOptimizerCursor( ns(), BSON( "_id" << GT << 5 << "a" << GT << 5 ) );
                ASSERT( c->ok() );

                // _id 10 {_id:1}
                ASSERT_EQUALS( 10, c->current().getIntField( "_id" ) );
                ASSERT( !c->matcher()->matchesCurrent( c.get() ) );
                ASSERT( c->advance() );

                // _id 0 {a:1}
                ASSERT_EQUALS( 0, c->current().getIntField( "_id" ) );
                ASSERT( !c->matcher()->matchesCurrent( c.get() ) );
                ASSERT( c->advance() );

                // _id 0 {$natural:1}
                ASSERT_EQUALS( 0, c->current().getIntField( "_id" ) );
                ASSERT( !c->matcher()->matchesCurrent( c.get() ) );
                ASSERT( c->advance() );
                
                // _id 11 {_id:1}
                ASSERT_EQUALS( BSON( "_id" << 11 << "a" << 12 ), c->current() );
                ASSERT( c->matcher()->matchesCurrent( c.get() ) );
                ASSERT( !c->getsetdup( c->currLoc() ) );
                ASSERT( c->advance() );
                
                // _id 12 {a:1}
                ASSERT_EQUALS( BSON( "_id" << 12 << "a" << 11 ), c->current() );
                ASSERT( c->matcher()->matchesCurrent( c.get() ) );
                ASSERT( !c->getsetdup( c->currLoc() ) );
                ASSERT( c->advance() );

                // _id 10 {$natural:1}
                ASSERT_EQUALS( 10, c->current().getIntField( "_id" ) );
                ASSERT( !c->matcher()->matchesCurrent( c.get() ) );
                ASSERT( c->advance() );
                                
                // _id 12 {_id:1}
                ASSERT_EQUALS( BSON( "_id" << 12 << "a" << 11 ), c->current() );
                ASSERT( c->matcher()->matchesCurrent( c.get() ) );
                ASSERT( c->getsetdup( c->currLoc() ) );
                ASSERT( c->advance() );
                
                // _id 11 {a:1}
                ASSERT_EQUALS( BSON( "_id" << 11 << "a" << 12 ), c->current() );
                ASSERT( c->matcher()->matchesCurrent( c.get() ) );
                ASSERT( c->getsetdup( c->currLoc() ) );
                ASSERT( c->advance() );

                // _id 11 {$natural:1}
                ASSERT_EQUALS( 11, c->current().getIntField( "_id" ) );
                ASSERT( c->matcher()->matchesCurrent( c.get() ) );
                ASSERT( c->getsetdup( c->currLoc() ) );

                // {_id:1} scan is complete.
                ASSERT( !c->advance() );
                ASSERT( !c->ok() );       
                
                // Scan the results again - this time the winning plan has been
                // recorded.
                c = newQueryOptimizerCursor( ns(), BSON( "_id" << GT << 5 << "a" << GT << 5 ) );
                ASSERT( c->ok() );

                // _id 10 {_id:1}
                ASSERT_EQUALS( 10, c->current().getIntField( "_id" ) );
                ASSERT( !c->matcher()->matchesCurrent( c.get() ) );
                ASSERT( c->advance() );
                
                // _id 11 {_id:1}
                ASSERT_EQUALS( BSON( "_id" << 11 << "a" << 12 ), c->current() );
                ASSERT( c->matcher()->matchesCurrent( c.get() ) );
                ASSERT( !c->getsetdup( c->currLoc() ) );
                ASSERT( c->advance() );
                
                // _id 12 {_id:1}
                ASSERT_EQUALS( BSON( "_id" << 12 << "a" << 11 ), c->current() );
                ASSERT( c->matcher()->matchesCurrent( c.get() ) );
                ASSERT( !c->getsetdup( c->currLoc() ) );
                
                // {_id:1} scan complete
                ASSERT( !c->advance() );
                ASSERT( !c->ok() );
            }
        };

        /** Curr key must be correct for currLoc for correct matching. */
        class ManualMatchingUsingCurrKey : public Base {
        public:
            void run() {
                _cli.insert( ns(), BSON( "_id" << "a" ) );
                _cli.insert( ns(), BSON( "_id" << "b" ) );
                _cli.insert( ns(), BSON( "_id" << "ba" ) );
                
                dblock lk;
                Client::Context ctx( ns() );
                shared_ptr< Cursor > c = newQueryOptimizerCursor( ns(), fromjson( "{_id:/a/}" ) );
                ASSERT( c->ok() );
                // "a"
                ASSERT( c->matcher()->matchesCurrent( c.get() ) );
                ASSERT( !c->getsetdup( c->currLoc() ) );
                ASSERT( c->advance() );
                ASSERT( c->ok() );
                
                // "b"
                ASSERT( !c->matcher()->matchesCurrent( c.get() ) );
                ASSERT( c->advance() );
                ASSERT( c->ok() );
                
                // "ba"
                ASSERT( c->matcher()->matchesCurrent( c.get() ) );
                ASSERT( !c->getsetdup( c->currLoc() ) );
                ASSERT( !c->advance() );
            }
        };
        
        /** Test matching and deduping done manually by cursor client. */
        class ManualMatchingDedupingTakeover : public Base {
        public:
            void run() {
                for( int i = 0; i < 150; ++i ) {
	                _cli.insert( ns(), BSON( "_id" << i << "a" << 0 ) );
                }
                _cli.insert( ns(), BSON( "_id" << 300 << "a" << 1 ) );
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                
                dblock lk;
                Client::Context ctx( ns() );
                shared_ptr< Cursor > c = newQueryOptimizerCursor( ns(), BSON( "$or" << BSON_ARRAY( BSON( "_id" << LT << 300 ) << BSON( "a" << 1 ) ) ) );
                for( int i = 0; i < 151; ++i ) {
                    ASSERT( c->ok() );
                    ASSERT( c->matcher()->matchesCurrent( c.get() ) );
                    ASSERT( !c->getsetdup( c->currLoc() ) );
                    c->advance();
                }
                ASSERT( !c->ok() );
            }
        };
        
        /** Test single key matching bounds. */
        class Singlekey : public Base {
        public:
            void run() {
                _cli.insert( ns(), BSON( "a" << "10" ) );
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                
                dblock lk;
                Client::Context ctx( ns() );
                shared_ptr< Cursor > c = newQueryOptimizerCursor( ns(), BSON( "a" << GT << 1 << LT << 5 ) );
                // Two sided bounds work.
                ASSERT( !c->ok() );
            }
        };

        /** Test multi key matching bounds. */
        class Multikey : public Base {
        public:
            void run() {
                _cli.insert( ns(), BSON( "a" << BSON_ARRAY( 1 << 10 ) ) );
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                
                dblock lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "a" << GT << 5 << LT << 3 ) );
                // Multi key bounds work.
                ASSERT( ok() );
            }
        };
        
        /** Add other plans when the recorded one is doing more poorly than expected. */
        class AddOtherPlans : public Base {
        public:
            void run() {
                _cli.insert( ns(), BSON( "_id" << 0 << "a" << 0 << "b" << 0 ) );
                _cli.insert( ns(), BSON( "_id" << 1 << "a" << 1 << "b" << 0 ) );
                for( int i = 100; i < 150; ++i ) {
                 	_cli.insert( ns(), BSON( "_id" << i << "a" << 100 << "b" << i ) );
                }
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                _cli.ensureIndex( ns(), BSON( "b" << 1 ) );
                
                dblock lk;
                Client::Context ctx( ns() );
                shared_ptr<Cursor> c = newQueryOptimizerCursor( ns(), BSON( "a" << 0 << "b" << 0 ) );
                
                ASSERT_EQUALS( BSON( "_id" << 0 << "a" << 0 << "b" << 0 ), c->current() );
                ASSERT( c->advance() );
                ASSERT_EQUALS( BSON( "_id" << 0 << "a" << 0 << "b" << 0 ), c->current() );
                ASSERT( c->advance() );
                // $natrual plan
                ASSERT_EQUALS( BSON( "_id" << 0 << "a" << 0 << "b" << 0 ), c->current() );                
                ASSERT( !c->advance() );
                
                c = newQueryOptimizerCursor( ns(), BSON( "a" << 100 << "b" << 149 ) );
                // Try {a:1}, which was successful previously.
                for( int i = 0; i < 11; ++i ) {
                 	ASSERT( 149 != c->current().getIntField( "b" ) );
                    ASSERT( c->advance() );
                }
                // Now try {b:1} plan.
                ASSERT_EQUALS( 149, c->current().getIntField( "b" ) );
                ASSERT( c->advance() );
                // {b:1} plan finished.
                ASSERT( !c->advance() );
            }
        };
        
        /** Check $or clause range elimination. */
        class OrRangeElimination : public Base {
        public:
            void run() {
                _cli.insert( ns(), BSON( "_id" << 1 ) );
                
                dblock lk;
                Client::Context ctx( ns() );
                shared_ptr<Cursor> c = newQueryOptimizerCursor( ns(), BSON( "$or" << BSON_ARRAY( BSON( "_id" << GT << 0 ) << BSON( "_id" << 1 ) ) ) );
                ASSERT( c->ok() );
                ASSERT( !c->advance() );
            }
        };
        
        /** Check $or match deduping - in takeover cursor. */
        class OrDedup : public Base {
        public:
            void run() {
                for( int i = 0; i < 150; ++i ) {
                 	_cli.insert( ns(), BSON( "_id" << i << "a" << i ) );   
                }
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                
                dblock lk;
                Client::Context ctx( ns() );
                shared_ptr<Cursor> c = newQueryOptimizerCursor( ns(), BSON( "$or" << BSON_ARRAY( BSON( "_id" << LT << 140 ) << BSON( "_id" << 145 ) << BSON( "a" << 145 ) ) ) );
                
                while( c->current().getIntField( "_id" ) < 140 ) {
                 	ASSERT( c->advance() );
                }
                // Match from second $or clause.
                ASSERT_EQUALS( 145, c->current().getIntField( "_id" ) );
                ASSERT( c->matcher()->matchesCurrent( c.get() ) );
                ASSERT( c->advance() );
                // Match from third $or clause.
                ASSERT_EQUALS( 145, c->current().getIntField( "_id" ) );
                // $or deduping is handled by the matcher.
                ASSERT( !c->matcher()->matchesCurrent( c.get() ) );
                ASSERT( !c->advance() );
            }
        };
        
        /** Standard dups with a multikey cursor. */
        class EarlyDups : public Base {
        public:
            void run() {
                _cli.insert( ns(), BSON( "a" << BSON_ARRAY( 0 << 1 << 200 ) ) );
                for( int i = 2; i < 150; ++i ) {
                 	_cli.insert( ns(), BSON( "a" << i ) );   
                }
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                
                dblock lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "a" << GT << -1 ) );
                ASSERT_EQUALS( 149, itcount() );
            }
        };
        
        /** Pop or clause in takeover cursor. */
        class OrPopInTakeover : public Base {
        public:
            void run() {
             	for( int i = 0; i < 150; ++i ) {
                 	_cli.insert( ns(), BSON( "_id" << i ) );   
                }
                
                dblock lk;
                Client::Context ctx( ns() );
                shared_ptr<Cursor> c = newQueryOptimizerCursor( ns(), BSON( "$or" << BSON_ARRAY( BSON( "_id" << LTE << 147 ) << BSON( "_id" << 148 ) << BSON( "_id" << 149 ) ) ) );
                for( int i = 0; i < 150; ++i ) {
                    ASSERT( c->ok() );
                 	ASSERT_EQUALS( i, c->current().getIntField( "_id" ) );
                    c->advance();
                }
                ASSERT( !c->ok() );
            }
        };
        
        /** Or clause iteration abandoned once full collection scan is performed. */
        class OrCollectionScanAbort : public Base {
        public:
            void run() {
                _cli.insert( ns(), BSON( "_id" << 0 << "a" << BSON_ARRAY( 1 << 2 << 3 << 4 << 5 ) << "b" << 4 ) );
                _cli.insert( ns(), BSON( "_id" << 1 << "a" << BSON_ARRAY( 6 << 7 << 8 << 9 << 10 ) << "b" << 4 ) );
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                
                dblock lk;
                Client::Context ctx( ns() );
                shared_ptr<Cursor> c = newQueryOptimizerCursor( ns(), BSON( "$or" << BSON_ARRAY( BSON( "a" << LT << 6 << "b" << 4 ) << BSON( "a" << GTE << 6 << "b" << 4 ) ) ) );
                
                ASSERT( c->ok() );
                
                // _id 0 on {a:1}
                ASSERT_EQUALS( 0, c->current().getIntField( "_id" ) );
                ASSERT( c->matcher()->matchesCurrent( c.get() ) );
                ASSERT( !c->getsetdup( c->currLoc() ) );
                c->advance();

                // _id 0 on {$natural:1}
                ASSERT_EQUALS( 0, c->current().getIntField( "_id" ) );
                ASSERT( c->matcher()->matchesCurrent( c.get() ) );
                ASSERT( c->getsetdup( c->currLoc() ) );
                c->advance();

                // _id 0 on {a:1}
                ASSERT_EQUALS( 0, c->current().getIntField( "_id" ) );
                ASSERT( c->matcher()->matchesCurrent( c.get() ) );
                ASSERT( c->getsetdup( c->currLoc() ) );
                c->advance();

                // _id 1 on {$natural:1}
                ASSERT_EQUALS( 1, c->current().getIntField( "_id" ) );
                ASSERT( c->matcher()->matchesCurrent( c.get() ) );
                ASSERT( !c->getsetdup( c->currLoc() ) );
                c->advance();

                // _id 0 on {a:1}
                ASSERT_EQUALS( 0, c->current().getIntField( "_id" ) );
                ASSERT( c->matcher()->matchesCurrent( c.get() ) );
                ASSERT( c->getsetdup( c->currLoc() ) );
                c->advance();
                
                // {$natural:1} finished
                ASSERT( !c->ok() );
            }
        };
        
        /** Simple geo query. */
        class Geo : public Base {
        public:
            void run() {
                _cli.insert( ns(), BSON( "_id" << 0 << "loc" << BSON( "lon" << 30 << "lat" << 30 ) ) );
                _cli.insert( ns(), BSON( "_id" << 1 << "loc" << BSON( "lon" << 31 << "lat" << 31 ) ) );
             	_cli.ensureIndex( ns(), BSON( "loc" << "2d" ) );
                
                dblock lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "loc" << BSON( "$near" << BSON_ARRAY( 30 << 30 ) ) ) );
                ASSERT( ok() );
                ASSERT_EQUALS( 0, current().getIntField( "_id" ) );
                ASSERT( advance() );
                ASSERT_EQUALS( 1, current().getIntField( "_id" ) );
                ASSERT( !advance() );
                ASSERT( !ok() );
            }
        };
        
        /** Yield cursor and delete current entry, then continue iteration. */
        class YieldNoOp : public Base {
        public:
            void run() {
             	_cli.insert( ns(), BSON( "_id" << 1 ) );
             	_cli.insert( ns(), BSON( "_id" << 2 ) );
                
                {
                    dblock lk;
                    Client::Context ctx( ns() );
                    setQueryOptimizerCursor( BSON( "_id" << GT << 0 ) );
                    ASSERT_EQUALS( 1, current().getIntField( "_id" ) );
                    ASSERT( prepareToYield() );
                }
                
                {
                    dblock lk;
                    Client::Context ctx( ns() );
                    recoverFromYield();
                    ASSERT( ok() );
                    ASSERT_EQUALS( 2, current().getIntField( "_id" ) );
                    ASSERT( !advance() );
                    ASSERT( !ok() );
                    ASSERT( prepareToYield() );
                    recoverFromYield();
                }
            }            
        };
        
        /** Yield cursor and delete current entry. */
        class YieldDelete : public Base {
        public:
            void run() {
             	_cli.insert( ns(), BSON( "_id" << 1 ) );
                
                {
                    dblock lk;
                    Client::Context ctx( ns() );
                    setQueryOptimizerCursor( BSON( "_id" << 1 ) );
                    ASSERT_EQUALS( 1, current().getIntField( "_id" ) );
                    ASSERT( prepareToYield() );
                }
                
                _cli.remove( ns(), BSON( "_id" << 1 ) );
                
                {
                    dblock lk;
                    Client::Context ctx( ns() );
                    recoverFromYield();
                    ASSERT( !ok() );
                    ASSERT( !advance() );
                }
            }
        };
        
        /** Yield cursor and delete current entry, then continue iteration. */
        class YieldDeleteContinue : public Base {
        public:
            void run() {
             	_cli.insert( ns(), BSON( "_id" << 1 ) );
             	_cli.insert( ns(), BSON( "_id" << 2 ) );
                
                {
                    dblock lk;
                    Client::Context ctx( ns() );
                    setQueryOptimizerCursor( BSON( "_id" << GT << 0 ) );
                    ASSERT_EQUALS( 1, current().getIntField( "_id" ) );
                    ASSERT( prepareToYield() );
                }
                
                _cli.remove( ns(), BSON( "_id" << 1 ) );
                
                {
                    dblock lk;
                    Client::Context ctx( ns() );
                    recoverFromYield();
                    ASSERT( ok() );
                    ASSERT_EQUALS( 2, current().getIntField( "_id" ) );
                    ASSERT( !advance() );
                    ASSERT( !ok() );
                }
            }            
        };

        /** Yield cursor and delete current entry, then continue iteration. */
        class YieldDeleteContinueFurther : public Base {
        public:
            void run() {
             	_cli.insert( ns(), BSON( "_id" << 1 ) );
             	_cli.insert( ns(), BSON( "_id" << 2 ) );
             	_cli.insert( ns(), BSON( "_id" << 3 ) );
                
                {
                    dblock lk;
                    Client::Context ctx( ns() );
                    setQueryOptimizerCursor( BSON( "_id" << GT << 0 ) );
                    ASSERT_EQUALS( 1, current().getIntField( "_id" ) );
                    ASSERT( prepareToYield() );
                }
                
                _cli.remove( ns(), BSON( "_id" << 1 ) );
                
                {
                    dblock lk;
                    Client::Context ctx( ns() );
                    recoverFromYield();
                    ASSERT( ok() );
                    ASSERT_EQUALS( 2, current().getIntField( "_id" ) );
                    ASSERT( advance() );
                    ASSERT_EQUALS( 3, current().getIntField( "_id" ) );
                    ASSERT( !advance() );
                    ASSERT( !ok() );
                }
            }            
        };
        
        /** Yield and update current. */
        class YieldUpdate : public Base {
        public:
            void run() {
             	_cli.insert( ns(), BSON( "a" << 1 ) );
             	_cli.insert( ns(), BSON( "a" << 2 ) );
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                
                {
                    dblock lk;
                    Client::Context ctx( ns() );
                    setQueryOptimizerCursor( BSON( "a" << GT << 0 ) );
                    ASSERT_EQUALS( 1, current().getIntField( "a" ) );
                    ASSERT( prepareToYield() );
                }
                
                _cli.update( ns(), BSON( "a" << 1 ), BSON( "$set" << BSON( "a" << 3 ) ) );
                
                {
                    dblock lk;
                    Client::Context ctx( ns() );
                    recoverFromYield();
                    ASSERT( ok() );
                    ASSERT_EQUALS( 2, current().getIntField( "a" ) );
                    ASSERT( !advance() );
                    ASSERT( !ok() );
                }                
            }
        };
        
        /** Yield and drop collection. */
        class YieldDrop : public Base {
        public:
            void run() {
                _cli.insert( ns(), BSON( "_id" << 1 ) );
             	_cli.insert( ns(), BSON( "_id" << 2 ) );
                
                {
                    dblock lk;
                    Client::Context ctx( ns() );
                    setQueryOptimizerCursor( BSON( "_id" << GT << 0 ) );
                    ASSERT_EQUALS( 1, current().getIntField( "_id" ) );
                    ASSERT( prepareToYield() );
                }
                
                _cli.dropCollection( ns() );
                
                {
                    dblock lk;
                    Client::Context ctx( ns() );
                    recoverFromYield();
                    ASSERT( !ok() );
                }                
            }
        };
        
        /** Yield and overwrite current in capped collection. */
        class YieldCappedOverwrite : public Base {
        public:
            void run() {
                _cli.createCollection( ns(), 1000, true );
                _cli.insert( ns(), BSON( "_id" << 1 ) );
                
                {
                    dblock lk;
                    Client::Context ctx( ns() );
                    setQueryOptimizerCursor( BSON( "_id" << GT << 0 ) );
                    ASSERT_EQUALS( 1, current().getIntField( "_id" ) );
                    ASSERT( prepareToYield() );
                }
                
                while( _cli.count( ns(), BSON( "_id" << 1 ) ) > 0 ) {
                 	_cli.insert( ns(), BSONObj() );   
                }

                {
                    dblock lk;
                    Client::Context ctx( ns() );
                    ASSERT_EXCEPTION( recoverFromYield(), MsgAssertionException );
                    ASSERT( !ok() );
                }                
            }
        };
        
        /** Yield and drop unrelated index - see SERVER-2454. */
        class YieldDropIndex : public Base {
        public:
            void run() {
                _cli.insert( ns(), BSON( "_id" << 1 ) );
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                
                {
                    dblock lk;
                    Client::Context ctx( ns() );
                    setQueryOptimizerCursor( BSON( "_id" << 1 ) );
                    ASSERT_EQUALS( 1, current().getIntField( "_id" ) );
                    ASSERT( prepareToYield() );
                }
                
                _cli.dropIndex( ns(), BSON( "a" << 1 ) );

                {
                    dblock lk;
                    Client::Context ctx( ns() );
                    recoverFromYield();
                    ASSERT( !ok() );
                }                
            }
        };

        /** Yielding with multiple plans active. */
        class YieldMultiplePlansNoOp : public Base {
        public:
            void run() {
                _cli.insert( ns(), BSON( "_id" << 1 << "a" << 2 ) );
             	_cli.insert( ns(), BSON( "_id" << 2 << "a" << 1 ) );
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                
                {
                    dblock lk;
                    Client::Context ctx( ns() );
                    setQueryOptimizerCursor( BSON( "_id" << GT << 0 << "a" << GT << 0 ) );
                    ASSERT_EQUALS( 1, current().getIntField( "_id" ) );
                    ASSERT( prepareToYield() );
                }
                
                {
                    dblock lk;
                    Client::Context ctx( ns() );
                    recoverFromYield();
                    ASSERT( ok() );
                    ASSERT_EQUALS( 2, current().getIntField( "_id" ) );
                    ASSERT( !advance() );
                    ASSERT( !ok() );
                }                
            }
        };

        /** Yielding with advance and multiple plans active. */
        class YieldMultiplePlansAdvanceNoOp : public Base {
        public:
            void run() {
                _cli.insert( ns(), BSON( "_id" << 1 << "a" << 2 ) );
             	_cli.insert( ns(), BSON( "_id" << 2 << "a" << 1 ) );
             	_cli.insert( ns(), BSON( "_id" << 3 << "a" << 3 ) );
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                
                {
                    dblock lk;
                    Client::Context ctx( ns() );
                    setQueryOptimizerCursor( BSON( "_id" << GT << 0 << "a" << GT << 0 ) );
                    ASSERT_EQUALS( 1, current().getIntField( "_id" ) );
                    advance();
                    ASSERT_EQUALS( 2, current().getIntField( "_id" ) );
                    ASSERT( prepareToYield() );
                }
                
                {
                    dblock lk;
                    Client::Context ctx( ns() );
                    recoverFromYield();
                    ASSERT( ok() );
                    ASSERT_EQUALS( 3, current().getIntField( "_id" ) );
                    ASSERT( !advance() );
                    ASSERT( !ok() );
                }                
            }
        };

        /** Yielding with delete and multiple plans active. */
        class YieldMultiplePlansDelete : public Base {
        public:
            void run() {
                _cli.insert( ns(), BSON( "_id" << 1 << "a" << 2 ) );
             	_cli.insert( ns(), BSON( "_id" << 2 << "a" << 1 ) );
             	_cli.insert( ns(), BSON( "_id" << 3 << "a" << 4 ) );
             	_cli.insert( ns(), BSON( "_id" << 4 << "a" << 3 ) );
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                
                {
                    dblock lk;
                    Client::Context ctx( ns() );
                    setQueryOptimizerCursor( BSON( "_id" << GT << 0 << "a" << GT << 0 ) );
                    ASSERT_EQUALS( 1, current().getIntField( "_id" ) );
                    advance();
                    ASSERT_EQUALS( 2, current().getIntField( "_id" ) );
                    ASSERT( prepareToYield() );
                }
                
                _cli.remove( ns(), BSON( "_id" << 2 ) );
                
                {
                    dblock lk;
                    Client::Context ctx( ns() );
                    c()->recoverFromYield();
                    ASSERT( ok() );
                    // index {a:1} active during yield
                    ASSERT_EQUALS( 1, current().getIntField( "_id" ) );
                    ASSERT( advance() );
                    ASSERT_EQUALS( 3, current().getIntField( "_id" ) );
                    ASSERT( advance() );
                    ASSERT_EQUALS( 4, current().getIntField( "_id" ) );
                    ASSERT( !advance() );
                    ASSERT( !ok() );
                }                
            }
        };

        /** Yielding with multiple plans and capped overwrite. */
        class YieldMultiplePlansCappedOverwrite : public Base {
        public:
            void run() {
                _cli.createCollection( ns(), 1000, true );
                _cli.insert( ns(), BSON( "_id" << 1 << "a" << 1 ) );
                _cli.ensureIndex( ns(), BSON( "_id" << 1 ) );
                
                {
                    dblock lk;
                    Client::Context ctx( ns() );
                    setQueryOptimizerCursor( BSON( "_id" << GT << 0 << "a" << GT << 0 ) );
                    ASSERT_EQUALS( 1, current().getIntField( "_id" ) );
                    ASSERT( prepareToYield() );
                }
                
                int i = 1;
                while( _cli.count( ns(), BSON( "_id" << 1 ) ) > 0 ) {
                    ++i;
                 	_cli.insert( ns(), BSON( "_id" << i << "a" << i ) );
                }
                
                {
                    dblock lk;
                    Client::Context ctx( ns() );
                    recoverFromYield();
                    ASSERT( ok() );
                    // {$natural:1} plan does not recover, {_id:1} plan does.
                    ASSERT( 1 < current().getIntField( "_id" ) );
                }                
            }
        };

        /**
         * Yielding with multiple plans and capped overwrite with unrecoverable cursor
         * active at time of yield.
         */
        class YieldMultiplePlansCappedOverwriteManual : public Base {
        public:
            void run() {
                _cli.createCollection( ns(), 1000, true );
                _cli.insert( ns(), BSON( "_id" << 1 << "a" << 1 ) );
                _cli.ensureIndex( ns(), BSON( "_id" << 1 ) );
                
                shared_ptr<Cursor> c;
                {
                    dblock lk;
                    Client::Context ctx( ns() );
                    c = newQueryOptimizerCursor( ns(), BSON( "_id" << GT << 0 << "a" << GT << 0 ) );
                    ASSERT_EQUALS( 1, c->current().getIntField( "_id" ) );
                    ASSERT( !c->getsetdup( c->currLoc() ) );
                    c->advance();
                    ASSERT_EQUALS( 1, c->current().getIntField( "_id" ) );
                    ASSERT( c->getsetdup( c->currLoc() ) );
                    ASSERT( c->prepareToYield() );
                }
                
                int i = 1;
                while( _cli.count( ns(), BSON( "_id" << 1 ) ) > 0 ) {
                    ++i;
                 	_cli.insert( ns(), BSON( "_id" << i << "a" << i ) );
                }
                
                {
                    dblock lk;
                    Client::Context ctx( ns() );
                    c->recoverFromYield();
                    ASSERT( c->ok() );
                    // {$natural:1} plan does not recover, {_id:1} plan does.
                    ASSERT( 1 < c->current().getIntField( "_id" ) );
                }                
            }
        };

        /**
         * Yielding with multiple plans and capped overwrite with unrecoverable cursor
         * inctive at time of yield.
         */
        class YieldMultiplePlansCappedOverwriteManual2 : public Base {
        public:
            void run() {
                _cli.createCollection( ns(), 1000, true );
                _cli.insert( ns(), BSON( "_id" << 1 << "a" << 1 ) );
                _cli.ensureIndex( ns(), BSON( "_id" << 1 ) );
                
                shared_ptr<Cursor> c;
                {
                    dblock lk;
                    Client::Context ctx( ns() );
                    c = newQueryOptimizerCursor( ns(), BSON( "_id" << GT << 0 << "a" << GT << 0 ) );
                    ASSERT_EQUALS( 1, c->current().getIntField( "_id" ) );
                    ASSERT( !c->getsetdup( c->currLoc() ) );
                    ASSERT( c->prepareToYield() );
                }
                
                int n = 1;
                while( _cli.count( ns(), BSON( "_id" << 1 ) ) > 0 ) {
                    ++n;
                 	_cli.insert( ns(), BSON( "_id" << n << "a" << n ) );
                }
                
                {
                    dblock lk;
                    Client::Context ctx( ns() );
                    c->recoverFromYield();
                    ASSERT( c->ok() );
                    // {$natural:1} plan does not recover, {_id:1} plan does.
                    ASSERT( 1 < c->current().getIntField( "_id" ) );
                    ASSERT( !c->getsetdup( c->currLoc() ) );
                    int i = c->current().getIntField( "_id" );
                    ASSERT( c->advance() );
                    ASSERT( c->getsetdup( c->currLoc() ) );
                    while( i < n ) {
                        ASSERT( c->advance() );
                        ++i;
                        ASSERT_EQUALS( i, c->current().getIntField( "_id" ) );
                    }
                }                
            }
        };
        
        /** Try and fail to yield a geo query. */
        class TryYieldGeo : public Base {
        public:
            void run() {
                _cli.insert( ns(), BSON( "_id" << 0 << "loc" << BSON( "lon" << 30 << "lat" << 30 ) ) );
             	_cli.ensureIndex( ns(), BSON( "loc" << "2d" ) );
                
                dblock lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "loc" << BSON( "$near" << BSON_ARRAY( 50 << 50 ) ) ) );
                ASSERT( ok() );
                ASSERT_EQUALS( 0, current().getIntField( "_id" ) );
                ASSERT( !prepareToYield() );
                ASSERT( ok() );
                ASSERT_EQUALS( 0, current().getIntField( "_id" ) );
                ASSERT( !advance() );
                ASSERT( !ok() );
            }
        };
        
        /** Yield with takeover cursor. */
        class YieldTakeover : public Base {
        public:
            void run() {
             	for( int i = 0; i < 150; ++i ) {
                 	_cli.insert( ns(), BSON( "_id" << i << "a" << i ) );   
                }
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                
                {
                    dblock lk;
                    Client::Context ctx( ns() );
                    setQueryOptimizerCursor( BSON( "_id" << GTE << 0 << "a" << GTE << 0 ) );
                    for( int i = 0; i < 120; ++i ) {
                     	ASSERT( advance() );
                    }
                    ASSERT( ok() );
                    ASSERT_EQUALS( 120, current().getIntField( "_id" ) );
                    ASSERT( prepareToYield() );
                }
                
                _cli.remove( ns(), BSON( "_id" << 120 ) );
                
                {
                    dblock lk;
                    Client::Context ctx( ns() );
                    recoverFromYield();
                    ASSERT( ok() );
                    ASSERT_EQUALS( 121, current().getIntField( "_id" ) );
                    ASSERT( advance() );
                    ASSERT_EQUALS( 122, current().getIntField( "_id" ) );
                }
            }
        };

        /** Yield with BacicCursor takeover cursor. */
        class YieldTakeoverBasic : public Base {
        public:
            void run() {
             	for( int i = 0; i < 150; ++i ) {
                 	_cli.insert( ns(), BSON( "_id" << i << "a" << BSON_ARRAY( i << i+1 ) ) );   
                }
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                
                auto_ptr<ClientCursor> cc;
                auto_ptr<ClientCursor::YieldData> data( new ClientCursor::YieldData() );
                {
                    dblock lk;
                    Client::Context ctx( ns() );
                    setQueryOptimizerCursor( BSON( "b" << NE << 0 << "a" << GTE << 0 ) );
                    cc.reset( new ClientCursor( QueryOption_NoCursorTimeout, c(), ns() ) );
                    for( int i = 0; i < 120; ++i ) {
                     	ASSERT( advance() );
                    }
                    ASSERT( ok() );
                    ASSERT_EQUALS( 120, current().getIntField( "_id" ) );
                    cc->prepareToYield( *data );
                }                
                _cli.remove( ns(), BSON( "_id" << 120 ) );
                
                {
                    dblock lk;
                    Client::Context ctx( ns() );
                    ASSERT( ClientCursor::recoverFromYield( *data ) );
                    ASSERT( ok() );
                    ASSERT_EQUALS( 121, current().getIntField( "_id" ) );
                    ASSERT( advance() );
                    ASSERT_EQUALS( 122, current().getIntField( "_id" ) );
                }
            }
        };
        
        /** Yield with advance of inactive cursor. */
        class YieldInactiveCursorAdvance : public Base {
        public:
            void run() {
                for( int i = 0; i < 10; ++i ) {
                    _cli.insert( ns(), BSON( "_id" << i << "a" << 10 - i ) );
                }
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                
                {
                    dblock lk;
                    Client::Context ctx( ns() );
                    setQueryOptimizerCursor( BSON( "_id" << GT << 0 << "a" << GT << 0 ) );
                    ASSERT( ok() );
                    ASSERT_EQUALS( 1, current().getIntField( "_id" ) );
                    ASSERT( advance() );
                    ASSERT_EQUALS( 9, current().getIntField( "_id" ) );
                    ASSERT( advance() );
                    ASSERT_EQUALS( 2, current().getIntField( "_id" ) );
                    ASSERT( prepareToYield() );
                }
                
                _cli.remove( ns(), BSON( "_id" << 9 ) );
                
                {
                    dblock lk;
                    Client::Context ctx( ns() );
                    recoverFromYield();
                    ASSERT( ok() );
                    ASSERT_EQUALS( 8, current().getIntField( "_id" ) );
                    ASSERT( advance() );
                    ASSERT_EQUALS( 3, current().getIntField( "_id" ) );
                    ASSERT( advance() );
                    ASSERT_EQUALS( 7, current().getIntField( "_id" ) );
                }                    
            }
        };
        
        class OrderId : public Base {
        public:
            void run() {
                for( int i = 0; i < 10; ++i ) {
                    _cli.insert( ns(), BSON( "_id" << i ) );
                }

                dblock lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSONObj(), BSON( "_id" << 1 ) );

                for( int i = 0; i < 10; ++i, advance() ) {
                    ASSERT( ok() );
                    ASSERT_EQUALS( i, current().getIntField( "_id" ) );
                }
            }
        };
        
        class OrderMultiIndex : public Base {
        public:
            void run() {
                for( int i = 0; i < 10; ++i ) {
                    _cli.insert( ns(), BSON( "_id" << i << "a" << 1 ) );
                }
                _cli.ensureIndex( ns(), BSON( "_id" << 1 << "a" << 1 ) );
                
                dblock lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "_id" << GTE << 0 << "a" << GTE << 0 ), BSON( "_id" << 1 ) );
                
                for( int i = 0; i < 10; ++i, advance() ) {
                    ASSERT( ok() );
                    ASSERT_EQUALS( i, current().getIntField( "_id" ) );
                }
            }
        };
        
        class OrderReject : public Base {
        public:
            void run() {
                for( int i = 0; i < 10; ++i ) {
                    _cli.insert( ns(), BSON( "_id" << i << "a" << i % 5 ) );
                }
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                
                dblock lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "a" << GTE << 3 ), BSON( "_id" << 1 ) );

                ASSERT( ok() );
                ASSERT_EQUALS( 3, current().getIntField( "_id" ) );
                ASSERT( advance() );
                ASSERT_EQUALS( 4, current().getIntField( "_id" ) );
                ASSERT( advance() );
                ASSERT_EQUALS( 8, current().getIntField( "_id" ) );
                ASSERT( advance() );
                ASSERT_EQUALS( 9, current().getIntField( "_id" ) );
                ASSERT( !advance() );
            }
        };

        class OrderNatural : public Base {
        public:
            void run() {
                _cli.insert( ns(), BSON( "_id" << 5 ) );
                _cli.insert( ns(), BSON( "_id" << 4 ) );
                _cli.insert( ns(), BSON( "_id" << 6 ) );
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                
                dblock lk;
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "_id" << GT << 0 ), BSON( "$natural" << 1 ) );
                
                ASSERT( ok() );
                ASSERT_EQUALS( 5, current().getIntField( "_id" ) );
                ASSERT( advance() );
                ASSERT_EQUALS( 4, current().getIntField( "_id" ) );
                ASSERT( advance() );                
                ASSERT_EQUALS( 6, current().getIntField( "_id" ) );
                ASSERT( !advance() );                
            }
        };
        
        class OrderUnindexed : public Base {
        public:
            void run() {
                dblock lk;
                Client::Context ctx( ns() );
             	ASSERT( !newQueryOptimizerCursor( ns(), BSONObj(), BSON( "a" << 1 ) ).get() );
            }
        };
        
        class RecordedOrderInvalid : public Base {
        public:
            void run() {
                _cli.insert( ns(), BSON( "a" << 1 << "b" << 1 ) );
                _cli.insert( ns(), BSON( "a" << 2 << "b" << 2 ) );
                _cli.insert( ns(), BSON( "a" << 3 << "b" << 3 ) );
                _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                _cli.ensureIndex( ns(), BSON( "b" << 1 ) );
                ASSERT( _cli.query( ns(), QUERY( "a" << 2 ).sort( "b" ) )->more() );
                
                dblock lk;
                Client::Context ctx( ns() );
                shared_ptr<Cursor> c = newQueryOptimizerCursor( ns(), BSON( "a" << 2 ), BSON( "b" << 1 ) );
                // Check that we are scanning {b:1} not {a:1}.
                for( int i = 0; i < 3; ++i ) {
                 	ASSERT( c->ok() );
                    c->advance();
                }
                ASSERT( !c->ok() );
            }
        };
        
        class KillOp : public Base {
        public:
            void run() {
                _cli.insert( ns(), BSON( "_id" << 1 << "b" << 1 ) );
                _cli.insert( ns(), BSON( "_id" << 2 << "b" << 2 ) );
                _cli.ensureIndex( ns(), BSON( "b" << 1 ) );
                
                mongolock lk( false );
                Client::Context ctx( ns() );
                setQueryOptimizerCursor( BSON( "_id" << GT << 0 << "b" << GT << 0 ) );
                ASSERT( ok() );
                cc().curop()->kill();
                // First advance() call throws, subsequent calls just fail.
                ASSERT_EXCEPTION( advance(), MsgAssertionException );
                ASSERT( !advance() );
            }
        };

        class KillOpFirstClause : public Base {
        public:
            void run() {
                _cli.insert( ns(), BSON( "_id" << 1 << "b" << 1 ) );
                _cli.insert( ns(), BSON( "_id" << 2 << "b" << 2 ) );
                _cli.ensureIndex( ns(), BSON( "b" << 1 ) );
                
                mongolock lk( false );
                Client::Context ctx( ns() );
                shared_ptr<Cursor> c = newQueryOptimizerCursor( ns(), BSON( "$or" << BSON_ARRAY( BSON( "_id" << GT << 0 ) << BSON( "b" << GT << 0 ) ) ) );
                ASSERT( c->ok() );
                cc().curop()->kill();
                // First advance() call throws, subsequent calls just fail.
                ASSERT_EXCEPTION( c->advance(), MsgAssertionException );
                ASSERT( !c->advance() );
            }
        };
        
        class Nscanned : public Base {
        public:
            void run() {
                for( int i = 0; i < 120; ++i ) {
                    _cli.insert( ns(), BSON( "_id" << i << "a" << i ) );
                }
                
                dblock lk;
                Client::Context ctx( ns() );
                shared_ptr<Cursor> c = newQueryOptimizerCursor( ns(), BSON( "_id" << GTE << 0 << "a" << GTE << 0 ) );
                ASSERT( c->ok() );
                ASSERT_EQUALS( 2, c->nscanned() );
                c->advance();
                ASSERT( c->ok() );
                ASSERT_EQUALS( 2, c->nscanned() );
                c->advance();
                for( int i = 3; i < 222; ++i ) {
                    ASSERT( c->ok() );
                    c->advance();
                }
                ASSERT( !c->ok() );
            }
        };
        
        namespace GetCursor {
            
            class Base : public QueryOptimizerCursorTests::Base {
            public:
                Base() {
                    // create collection
                    _cli.insert( ns(), BSON( "_id" << 5 ) );
                }
                virtual ~Base() {}
                void run() {
                    dblock lk;
                    Client::Context ctx( ns() );
                    shared_ptr<Cursor> c = NamespaceDetailsTransient::getCursor( ns(), query(), order() );
                    string type = c->toString().substr( 0, expectedType().length() );
                    ASSERT_EQUALS( expectedType(), type );
                    check( c );
                }
            protected:
                virtual string expectedType() const = 0;
                virtual BSONObj query() const { return BSONObj(); }
                virtual BSONObj order() const { return BSONObj(); }
                virtual void check( const shared_ptr<Cursor> &c ) {
                    ASSERT( c->ok() );
                    ASSERT( !c->matcher() );
                    ASSERT_EQUALS( 5, c->current().getIntField( "_id" ) );
                    ASSERT( !c->advance() );
                }
            };
            
            class NoConstraints : public Base {
                string expectedType() const { return "BasicCursor"; }
            };

            class SimpleId : public Base {
            public:
                SimpleId() {
                    _cli.insert( ns(), BSON( "_id" << 0 ) );
                    _cli.insert( ns(), BSON( "_id" << 10 ) );
                }
                string expectedType() const { return "BtreeCursor _id_"; }
                BSONObj query() const { return BSON( "_id" << 5 ); }
            };

            class OptimalIndex : public Base {
            public:
                OptimalIndex() {
                    _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                    _cli.insert( ns(), BSON( "a" << 5 ) );
                    _cli.insert( ns(), BSON( "a" << 6 ) );
                }
                string expectedType() const { return "BtreeCursor a_1"; }
                BSONObj query() const { return BSON( "a" << GTE << 5 ); }
                void check( const shared_ptr<Cursor> &c ) {
                    ASSERT( c->ok() );
                    ASSERT( c->matcher() );
                    ASSERT_EQUALS( 5, c->current().getIntField( "a" ) );
                    ASSERT( c->matcher()->matchesCurrent( c.get() ) );
                    ASSERT( c->advance() );                    
                    ASSERT_EQUALS( 6, c->current().getIntField( "a" ) );
                    ASSERT( c->matcher()->matchesCurrent( c.get() ) );
                    ASSERT( !c->advance() );                    
                }
            };
            
            class Geo : public Base {
            public:
                Geo() {
                    _cli.insert( ns(), BSON( "_id" << 44 << "loc" << BSON_ARRAY( 44 << 45 ) ) );
                    _cli.ensureIndex( ns(), BSON( "loc" << "2d" ) );
                }
                string expectedType() const { return "GeoSearchCursor"; }
                BSONObj query() const { return fromjson( "{ loc : { $near : [50,50] } }" ); }
                void check( const shared_ptr<Cursor> &c ) {
                    ASSERT( c->ok() );
                    ASSERT( c->matcher() );
                    ASSERT( c->matcher()->matchesCurrent( c.get() ) );
                    ASSERT_EQUALS( 44, c->current().getIntField( "_id" ) );
                    ASSERT( !c->advance() );
                }
            };
            
            class OutOfOrder : public QueryOptimizerCursorTests::Base {
            public:
                void run() {
                    _cli.insert( ns(), BSON( "_id" << 5 ) );
                    dblock lk;
                    Client::Context ctx( ns() );
                    shared_ptr<Cursor> c = NamespaceDetailsTransient::getCursor( ns(), BSONObj(), BSON( "b" << 1 ) );
                    ASSERT( !c );
                }
            };
            
            class BestSavedOutOfOrder : public QueryOptimizerCursorTests::Base {
            public:
                void run() {
                    _cli.insert( ns(), BSON( "_id" << 5 << "b" << BSON_ARRAY( 1 << 2 << 3 << 4 << 5 ) ) );
                    _cli.insert( ns(), BSON( "_id" << 1 << "b" << 6 ) );
                    _cli.ensureIndex( ns(), BSON( "b" << 1 ) );
                    // record {_id:1} index for this query
                    ASSERT( _cli.query( ns(), QUERY( "_id" << GT << 0 << "b" << GT << 0 ).sort( "b" ) )->more() );
                    dblock lk;
                    Client::Context ctx( ns() );
                    shared_ptr<Cursor> c = NamespaceDetailsTransient::getCursor( ns(), BSON( "_id" << GT << 0 << "b" << GT << 0 ), BSON( "b" << 1 ) );
                    // {_id:1} requires scan and order, so {b:1} must be chosen.
                    ASSERT( c );
                    ASSERT_EQUALS( 5, c->current().getIntField( "_id" ) );
                }
            };
            
            class MultiIndex : public Base {
            public:
                MultiIndex() {
                    _cli.ensureIndex( ns(), BSON( "a" << 1 ) );
                }
                string expectedType() const { return "QueryOptimizerCursor"; }
                BSONObj query() const { return BSON( "_id" << GT << 0 << "a" << GT << 0 ); }
                void check( const shared_ptr<Cursor> &c ) {}
            };
            
        } // namespace GetCursor
        
    } // namespace QueryOptimizerCursorTests

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
            add<BestGuess>();
            add<QueryOptimizerCursorTests::Empty>();
            add<QueryOptimizerCursorTests::Unindexed>();
            add<QueryOptimizerCursorTests::Basic>();
            add<QueryOptimizerCursorTests::NoMatch>();
            add<QueryOptimizerCursorTests::Interleaved>();
            add<QueryOptimizerCursorTests::NotMatch>();
            add<QueryOptimizerCursorTests::StopInterleaving>();
            add<QueryOptimizerCursorTests::TakeoverWithDup>();
            add<QueryOptimizerCursorTests::TakeoverWithNonMatches>();
            add<QueryOptimizerCursorTests::TakeoverWithTakeoverDup>();
            add<QueryOptimizerCursorTests::BasicOr>();
            add<QueryOptimizerCursorTests::OrFirstClauseEmpty>();
            add<QueryOptimizerCursorTests::OrSecondClauseEmpty>();
            add<QueryOptimizerCursorTests::OrMultipleClausesEmpty>();
            add<QueryOptimizerCursorTests::TakeoverCountOr>();
            add<QueryOptimizerCursorTests::TakeoverEndOfOrClause>();
            add<QueryOptimizerCursorTests::TakeoverBeforeEndOfOrClause>();
            add<QueryOptimizerCursorTests::TakeoverAfterEndOfOrClause>();
            add<QueryOptimizerCursorTests::ManualMatchingDeduping>();
            add<QueryOptimizerCursorTests::ManualMatchingUsingCurrKey>();
            add<QueryOptimizerCursorTests::ManualMatchingDedupingTakeover>();
            add<QueryOptimizerCursorTests::Singlekey>();
            add<QueryOptimizerCursorTests::Multikey>();
            add<QueryOptimizerCursorTests::AddOtherPlans>();
            add<QueryOptimizerCursorTests::OrRangeElimination>();
            add<QueryOptimizerCursorTests::OrDedup>();
            add<QueryOptimizerCursorTests::EarlyDups>();
            add<QueryOptimizerCursorTests::OrPopInTakeover>();
            add<QueryOptimizerCursorTests::OrCollectionScanAbort>();
            add<QueryOptimizerCursorTests::Geo>();
            add<QueryOptimizerCursorTests::YieldNoOp>();
            add<QueryOptimizerCursorTests::YieldDelete>();
            add<QueryOptimizerCursorTests::YieldDeleteContinue>();
            add<QueryOptimizerCursorTests::YieldDeleteContinueFurther>();
            add<QueryOptimizerCursorTests::YieldUpdate>();
            add<QueryOptimizerCursorTests::YieldDrop>();
            add<QueryOptimizerCursorTests::YieldCappedOverwrite>();
            add<QueryOptimizerCursorTests::YieldDropIndex>();
            add<QueryOptimizerCursorTests::YieldMultiplePlansNoOp>();
            add<QueryOptimizerCursorTests::YieldMultiplePlansAdvanceNoOp>();
            add<QueryOptimizerCursorTests::YieldMultiplePlansDelete>();
            add<QueryOptimizerCursorTests::YieldMultiplePlansCappedOverwrite>();
            add<QueryOptimizerCursorTests::YieldMultiplePlansCappedOverwriteManual>();
            add<QueryOptimizerCursorTests::YieldMultiplePlansCappedOverwriteManual2>();
            add<QueryOptimizerCursorTests::TryYieldGeo>();
            add<QueryOptimizerCursorTests::YieldTakeover>();
            add<QueryOptimizerCursorTests::YieldTakeoverBasic>();
            add<QueryOptimizerCursorTests::YieldInactiveCursorAdvance>();
            add<QueryOptimizerCursorTests::OrderId>();
            add<QueryOptimizerCursorTests::OrderMultiIndex>();
            add<QueryOptimizerCursorTests::OrderReject>();
            add<QueryOptimizerCursorTests::OrderNatural>();
            add<QueryOptimizerCursorTests::OrderUnindexed>();
            add<QueryOptimizerCursorTests::RecordedOrderInvalid>();
            add<QueryOptimizerCursorTests::KillOp>();
            add<QueryOptimizerCursorTests::KillOpFirstClause>();
            add<QueryOptimizerCursorTests::Nscanned>();
            add<QueryOptimizerCursorTests::GetCursor::NoConstraints>();
            add<QueryOptimizerCursorTests::GetCursor::SimpleId>();
            add<QueryOptimizerCursorTests::GetCursor::OptimalIndex>();
            add<QueryOptimizerCursorTests::GetCursor::Geo>();
            add<QueryOptimizerCursorTests::GetCursor::OutOfOrder>();
            add<QueryOptimizerCursorTests::GetCursor::BestSavedOutOfOrder>();
            add<QueryOptimizerCursorTests::GetCursor::MultiIndex>();
        }
    } myall;

} // namespace QueryOptimizerTests

