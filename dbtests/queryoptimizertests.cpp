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

#include "../db/queryoptimizer.h"

#include "../db/db.h"
#include "../db/dbhelpers.h"
#include "../db/instance.h"
#include "../db/query.h"

#include "dbtests.h"

namespace QueryOptimizerTests {

    namespace FieldBoundTests {
        class Base {
        public:
            virtual ~Base() {}
            void run() {
                FieldBoundSet s( "ns", query() );
                checkElt( lower(), s.bound( "a" ).lower() );
                checkElt( upper(), s.bound( "a" ).upper() );
            }
        protected:
            virtual BSONObj query() = 0;
            virtual BSONElement lower() { return minKey.firstElement(); }
            virtual BSONElement upper() { return maxKey.firstElement(); }
        private:
            static void checkElt( BSONElement expected, BSONElement actual ) {
                if ( expected.woCompare( actual, false ) ) {
                    stringstream ss;
                    ss << "expected: " << expected << ", got: " << actual;
                    FAIL( ss.str() );
                }
            }
        };
        
        class Empty : public Base {
            virtual BSONObj query() { return emptyObj; }
        };
        
        class Eq : public Base {
        public:
            Eq() : o_( BSON( "a" << 1 ) ) {}
            virtual BSONObj query() { return o_; }
            virtual BSONElement lower() { return o_.firstElement(); }
            virtual BSONElement upper() { return o_.firstElement(); }
            BSONObj o_;
        };

        class DupEq : public Eq {
        public:
            virtual BSONObj query() { return BSON( "a" << 1 << "b" << 2 << "a" << 1 ); }
        };        

        class Lt : public Base {
        public:
            Lt() : o_( BSON( "-" << 1 ) ) {}
            virtual BSONObj query() { return BSON( "a" << LT << 1 ); }
            virtual BSONElement upper() { return o_.firstElement(); }
            BSONObj o_;
        };        

        class Lte : public Lt {
            virtual BSONObj query() { return BSON( "a" << LTE << 1 ); }            
        };
        
        class Gt : public Base {
        public:
            Gt() : o_( BSON( "-" << 1 ) ) {}
            virtual BSONObj query() { return BSON( "a" << GT << 1 ); }
            virtual BSONElement lower() { return o_.firstElement(); }
            BSONObj o_;
        };        
        
        class Gte : public Gt {
            virtual BSONObj query() { return BSON( "a" << GTE << 1 ); }            
        };
        
        class TwoLt : public Lt {
            virtual BSONObj query() { return BSON( "a" << LT << 1 << LT << 5 ); }                        
        };

        class TwoGt : public Gt {
            virtual BSONObj query() { return BSON( "a" << GT << 0 << GT << 1 ); }                        
        };        
        
        class EqGte : public Eq {
            virtual BSONObj query() { return BSON( "a" << 1 << "a" << GTE << 1 ); }            
        };

        class EqGteInvalid {
        public:
            void run() {
                FieldBoundSet fbs( "ns", BSON( "a" << 1 << "a" << GTE << 2 ) );
                ASSERT( !fbs.matchPossible() );
            }
        };        

        class Regex : public Base {
        public:
            Regex() : o1_( BSON( "" << "abc" ) ), o2_( BSON( "" << "abd" ) ) {}
            virtual BSONObj query() {
                BSONObjBuilder b;
                b.appendRegex( "a", "^abc" );
                return b.obj();
            }
            virtual BSONElement lower() { return o1_.firstElement(); }
            virtual BSONElement upper() { return o2_.firstElement(); }
            BSONObj o1_, o2_;
        };        
        
        class UnhelpfulRegex : public Base {
            virtual BSONObj query() {
                BSONObjBuilder b;
                b.appendRegex( "a", "abc" );
                return b.obj();
            }            
        };
        
        class In : public Base {
        public:
            In() : o1_( BSON( "-" << -3 ) ), o2_( BSON( "-" << 44 ) ) {}
            virtual BSONObj query() {
                vector< int > vals;
                vals.push_back( 4 );
                vals.push_back( 8 );
                vals.push_back( 44 );
                vals.push_back( -1 );
                vals.push_back( -3 );
                vals.push_back( 0 );
                BSONObjBuilder bb;
                bb.append( "$in", vals );
                BSONObjBuilder b;
                b.append( "a", bb.done() );
                return b.obj();
            }
            virtual BSONElement lower() { return o1_.firstElement(); }
            virtual BSONElement upper() { return o2_.firstElement(); }
            BSONObj o1_, o2_;
        };
        
        class SimplifiedQuery {
        public:
            void run() {
                FieldBoundSet fbs( "ns", BSON( "a" << GT << 1 << GT << 5 << LT << 10 << "b" << 4 << "c" << LT << 4 << LT << 6 << "d" << GTE << 0 ) );
                BSONObj simple = fbs.simplifiedQuery();
                out() << "simple: " << simple << endl;
                ASSERT( !simple.getObjectField( "a" ).woCompare( fromjson( "{$gte:5,$lte:10}" ) ) );
                ASSERT_EQUALS( 4, simple.getIntField( "b" ) );
                ASSERT( !simple.getObjectField( "c" ).woCompare( fromjson( "{$lte:4}" ) ) );
                ASSERT( !simple.getObjectField( "d" ).woCompare( fromjson( "{$gte:0}" ) ) );
            }
        };
        
        class QueryPatternTest {
        public:
            void run() {
                ASSERT( p( BSON( "a" << 1 ) ) == p( BSON( "a" << 1 ) ) );
                ASSERT( p( BSON( "a" << 1 ) ) == p( BSON( "a" << 5 ) ) );
                ASSERT( p( BSON( "a" << 1 ) ) != p( BSON( "b" << 1 ) ) );
                ASSERT( p( BSON( "a" << 1 ) ) != p( BSON( "a" << LTE << 1 ) ) );
                ASSERT( p( BSON( "a" << 1 ) ) != p( BSON( "a" << 1 << "b" << 2 ) ) );
                ASSERT( p( BSON( "a" << 1 << "b" << 3 ) ) != p( BSON( "a" << 1 ) ) );
                ASSERT( p( BSON( "a" << LT << 1 ) ) == p( BSON( "a" << LTE << 5 ) ) );
                ASSERT( p( BSON( "a" << LT << 1 << GTE << 0 ) ) == p( BSON( "a" << LTE << 5 << GTE << 0 ) ) );
            }
        private:
            static QueryPattern p( const BSONObj &query ) {
                return FieldBoundSet( "", query ).pattern();
            }
        };
        
    } // namespace FieldBoundTests
    
    namespace QueryPlanTests {
        class Base {
        public:
            Base() : indexNum_( 0 ) {
                setClient( ns() );
                string err;
                userCreateNS( ns(), emptyObj, err, false );
            }
            ~Base() {
                if ( !nsd() )
                    return;
                string s( ns() );
                dropNS( s );
            }
        protected:
            static const char *ns() { return "QueryPlanTests.coll"; }
            static NamespaceDetails *nsd() { return nsdetails( ns() ); }
            const IndexDetails *index( const BSONObj &key ) {
                dbtemprelease r;
                stringstream ss;
                ss << indexNum_++;
                string name = ss.str();
                client_.resetIndexCache();
                client_.ensureIndex( ns(), key, name.c_str() );
                NamespaceDetails *d = nsd();
                for( int i = 0; i < d->nIndexes; ++i ) {
                    if ( d->indexes[ i ].indexName() == name )
                        return &d->indexes[ i ];
                }
                assert( false );
                return 0;
            }
        private:
            dblock lk_;
            int indexNum_;
            static DBDirectClient client_;
        };
        DBDirectClient Base::client_;
        
        // There's a limit of 10 indexes total, make sure not to exceed this in a given test.
#define INDEX(x) this->index( BSON(x) )
        FieldBoundSet FieldBoundSet_GLOBAL( "", emptyObj );
#define FBS(x) ( FieldBoundSet_GLOBAL = FieldBoundSet( ns(), x ) )
        
        class NoIndex : public Base {
        public:
            void run() {
                QueryPlan p( FBS( emptyObj ), emptyObj, 0 );
                ASSERT( !p.optimal() );
                ASSERT( !p.scanAndOrderRequired() );
                ASSERT( !p.keyMatch() );
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
                
                QueryPlan p( FBS( emptyObj ), BSON( "a" << 1 ), INDEX( "a" << 1 ) );
                ASSERT( !p.scanAndOrderRequired() );
                ASSERT( !p.startKey().woCompare( start ) );
                ASSERT( !p.endKey().woCompare( end ) );
                QueryPlan p2( FBS( emptyObj ), BSON( "a" << 1 << "b" << 1 ), INDEX( "a" << 1 << "b" << 1 ) );
                ASSERT( !p2.scanAndOrderRequired() );
                QueryPlan p3( FBS( emptyObj ), BSON( "b" << 1 ), INDEX( "a" << 1 ) );
                ASSERT( p3.scanAndOrderRequired() );
                ASSERT( !p3.startKey().woCompare( start ) );
                ASSERT( !p3.endKey().woCompare( end ) );
            }
        };
        
        class MoreIndexThanNeeded : public Base {
        public:
            void run() {
                QueryPlan p( FBS( emptyObj ), BSON( "a" << 1 ), INDEX( "a" << 1 << "b" << 1 ) );
                ASSERT( !p.scanAndOrderRequired() );                
            }
        };
        
        class IndexSigns : public Base {
        public:
            void run() {
                QueryPlan p( FBS( emptyObj ), BSON( "a" << 1 << "b" << -1 ), INDEX( "a" << 1 << "b" << -1 ) );
                ASSERT( !p.scanAndOrderRequired() );                
                ASSERT_EQUALS( 1, p.direction() );
                QueryPlan p2( FBS( emptyObj ), BSON( "a" << 1 << "b" << -1 ), INDEX( "a" << 1 << "b" << 1 ) );
                ASSERT( p2.scanAndOrderRequired() );                
                ASSERT_EQUALS( 0, p2.direction() );
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
                QueryPlan p( FBS( emptyObj ), BSON( "a" << 1 << "b" << -1 ), INDEX( "a" << -1 << "b" << 1 ) );
                ASSERT( !p.scanAndOrderRequired() );                
                ASSERT_EQUALS( -1, p.direction() );
                ASSERT( !p.startKey().woCompare( start ) );
                ASSERT( !p.endKey().woCompare( end ) );
                QueryPlan p2( FBS( emptyObj ), BSON( "a" << -1 << "b" << -1 ), INDEX( "a" << 1 << "b" << 1 ) );
                ASSERT( !p2.scanAndOrderRequired() );                
                ASSERT_EQUALS( -1, p2.direction() );
                QueryPlan p3( FBS( emptyObj ), BSON( "a" << -1 << "b" << -1 ), INDEX( "a" << 1 << "b" << -1 ) );
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
                QueryPlan p( FBS( BSON( "a" << 3 ) ), emptyObj, INDEX( "a" << -1 << "b" << 1 ) );
                ASSERT( !p.scanAndOrderRequired() );                
                ASSERT( !p.startKey().woCompare( start ) );
                ASSERT( !p.endKey().woCompare( end ) );
                QueryPlan p2( FBS( BSON( "a" << 3 ) ), BSONObj(), INDEX( "a" << -1 << "b" << 1 ) );
                ASSERT( !p2.scanAndOrderRequired() );                
                ASSERT( !p.startKey().woCompare( start ) );
                ASSERT( !p.endKey().woCompare( end ) );
            }            
        };
        
        class EqualWithOrder : public Base {
        public:
            void run() {
                QueryPlan p( FBS( BSON( "a" << 4 ) ), BSON( "b" << 1 ), INDEX( "a" << 1 << "b" << 1 ) );
                ASSERT( !p.scanAndOrderRequired() );                
                QueryPlan p2( FBS( BSON( "b" << 4 ) ), BSON( "a" << 1 << "c" << 1 ), INDEX( "a" << 1 << "b" << 1 << "c" << 1 ) );
                ASSERT( !p2.scanAndOrderRequired() );                
                QueryPlan p3( FBS( BSON( "b" << 4 ) ), BSON( "a" << 1 << "c" << 1 ), INDEX( "a" << 1 << "b" << 1 ) );
                ASSERT( p3.scanAndOrderRequired() );                
            }
        };
        
        class Optimal : public Base {
        public:
            void run() {
                QueryPlan p( FBS( emptyObj ), BSON( "a" << 1 ), INDEX( "a" << 1 ) );
                ASSERT( p.optimal() );
                QueryPlan p2( FBS( emptyObj ), BSON( "a" << 1 ), INDEX( "a" << 1 << "b" << 1 ) );
                ASSERT( p2.optimal() );
                QueryPlan p3( FBS( BSON( "a" << 1 ) ), BSON( "a" << 1 ), INDEX( "a" << 1 << "b" << 1 ) );
                ASSERT( p3.optimal() );
                QueryPlan p4( FBS( BSON( "b" << 1 ) ), BSON( "a" << 1 ), INDEX( "a" << 1 << "b" << 1 ) );
                ASSERT( !p4.optimal() );
                QueryPlan p5( FBS( BSON( "a" << 1 ) ), BSON( "b" << 1 ), INDEX( "a" << 1 << "b" << 1 ) );
                ASSERT( p5.optimal() );
                QueryPlan p6( FBS( BSON( "b" << 1 ) ), BSON( "b" << 1 ), INDEX( "a" << 1 << "b" << 1 ) );
                ASSERT( !p6.optimal() );
                QueryPlan p7( FBS( BSON( "a" << 1 << "b" << 1 ) ), BSON( "a" << 1 ), INDEX( "a" << 1 << "b" << 1 ) );
                ASSERT( p7.optimal() );
                QueryPlan p8( FBS( BSON( "a" << 1 << "b" << LT << 1 ) ), BSON( "a" << 1 ), INDEX( "a" << 1 << "b" << 1 ) );
                ASSERT( p8.optimal() );
                QueryPlan p9( FBS( BSON( "a" << 1 << "b" << LT << 1 ) ), BSON( "a" << 1 ), INDEX( "a" << 1 << "b" << 1 << "c" << 1 ) );
                ASSERT( p9.optimal() );
                QueryPlan p10( FBS( BSON( "a" << 1 ) ), emptyObj, INDEX( "a" << 1 << "b" << 1 << "c" << 1 ) );
                ASSERT( p10.optimal() );
            }
        };
        
        class MoreOptimal : public Base {
        public:
            void run() {
                QueryPlan p11( FBS( BSON( "a" << 1 << "b" << LT << 1 ) ), emptyObj, INDEX( "a" << 1 << "b" << 1 << "c" << 1 ) );
                ASSERT( p11.optimal() );
                QueryPlan p12( FBS( BSON( "a" << LT << 1 ) ), emptyObj, INDEX( "a" << 1 << "b" << 1 << "c" << 1 ) );
                ASSERT( p12.optimal() );
                QueryPlan p13( FBS( BSON( "a" << LT << 1 ) ), BSON( "a" << 1 ), INDEX( "a" << 1 << "b" << 1 << "c" << 1 ) );
                ASSERT( p13.optimal() );
            }
        };
        
        class KeyMatch : public Base {
        public:
            void run() {
                QueryPlan p( FBS( emptyObj ), BSON( "a" << 1 ), INDEX( "a" << 1 ) );
                ASSERT( p.keyMatch() );
                ASSERT( p.exactKeyMatch() );
                QueryPlan p2( FBS( emptyObj ), BSON( "a" << 1 ), INDEX( "b" << 1 << "a" << 1 ) );
                ASSERT( p2.keyMatch() );
                ASSERT( p2.exactKeyMatch() );
                QueryPlan p3( FBS( BSON( "b" << "z" ) ), BSON( "a" << 1 ), INDEX( "b" << 1 << "a" << 1 ) );
                ASSERT( p3.keyMatch() );
                ASSERT( p3.exactKeyMatch() );
                QueryPlan p4( FBS( BSON( "c" << "y" << "b" << "z" ) ), BSON( "a" << 1 ), INDEX( "b" << 1 << "a" << 1 << "c" << 1 ) );
                ASSERT( p4.keyMatch() );
                ASSERT( p4.exactKeyMatch() );
                QueryPlan p5( FBS( BSON( "c" << "y" << "b" << "z" ) ), emptyObj, INDEX( "b" << 1 << "a" << 1 << "c" << 1 ) );
                ASSERT( p5.keyMatch() );
                ASSERT( p5.exactKeyMatch() );
                QueryPlan p6( FBS( BSON( "c" << LT << "y" << "b" << GT << "z" ) ), emptyObj, INDEX( "b" << 1 << "a" << 1 << "c" << 1 ) );
                ASSERT( p6.keyMatch() );
                ASSERT( !p6.exactKeyMatch() );
                QueryPlan p7( FBS( emptyObj ), BSON( "a" << 1 ), INDEX( "b" << 1 ) );
                ASSERT( !p7.keyMatch() );
                ASSERT( !p7.exactKeyMatch() );
                QueryPlan p8( FBS( BSON( "d" << "y" ) ), BSON( "a" << 1 ), INDEX( "a" << 1 ) );
                ASSERT( !p8.keyMatch() );
                ASSERT( !p8.exactKeyMatch() );
            }
        };
        
        class ExactKeyQueryTypes : public Base {
        public:
            void run() {
                QueryPlan p( FBS( BSON( "a" << "b" ) ), emptyObj, INDEX( "a" << 1 ) );
                ASSERT( p.exactKeyMatch() );
                QueryPlan p2( FBS( BSON( "a" << 4 ) ), emptyObj, INDEX( "a" << 1 ) );
                ASSERT( !p2.exactKeyMatch() );
                QueryPlan p3( FBS( BSON( "a" << BSON( "c" << "d" ) ) ), emptyObj, INDEX( "a" << 1 ) );
                ASSERT( !p3.exactKeyMatch() );
                BSONObjBuilder b;
                b.appendRegex( "a", "^ddd" );
                QueryPlan p4( FBS( b.obj() ), emptyObj, INDEX( "a" << 1 ) );
                ASSERT( !p4.exactKeyMatch() );
                QueryPlan p5( FBS( BSON( "a" << "z" << "b" << 4 ) ), emptyObj, INDEX( "a" << 1 << "b" << 1 ) );
                ASSERT( !p5.exactKeyMatch() );
            }
        };
        
        class Unhelpful : public Base {
        public:
            void run() {
                QueryPlan p( FBS( BSON( "b" << 1 ) ), emptyObj, INDEX( "a" << 1 << "b" << 1 ) );
                ASSERT( p.keyMatch() );
                ASSERT( !p.bound( "a" ).nontrivial() );
                ASSERT( !p.unhelpful() );
                QueryPlan p2( FBS( BSON( "b" << 1 << "c" << 1 ) ), BSON( "a" << 1 ), INDEX( "a" << 1 << "b" << 1 ) );
                ASSERT( !p2.keyMatch() );
                ASSERT( !p2.scanAndOrderRequired() );
                ASSERT( !p2.bound( "a" ).nontrivial() );
                ASSERT( !p2.unhelpful() );
                QueryPlan p3( FBS( BSON( "b" << 1 << "c" << 1 ) ), emptyObj, INDEX( "b" << 1 ) );
                ASSERT( !p3.keyMatch() );
                ASSERT( p3.bound( "b" ).nontrivial() );
                ASSERT( !p3.unhelpful() );
                QueryPlan p4( FBS( BSON( "c" << 1 << "d" << 1 ) ), emptyObj, INDEX( "b" << 1 << "c" << 1 ) );
                ASSERT( !p4.keyMatch() );
                ASSERT( !p4.bound( "b" ).nontrivial() );
                ASSERT( p4.unhelpful() );
            }
        };
        
    } // namespace QueryPlanTests

    namespace QueryPlanSetTests {
        class Base {
        public:
            Base() {
                setClient( ns() );
                string err;
                userCreateNS( ns(), emptyObj, err, false );
                AuthenticationInfo *ai = new AuthenticationInfo();
                authInfo.reset( ai );
            }
            ~Base() {
                if ( !nsd() )
                    return;
                string s( ns() );
                dropNS( s );
            }
            static void assembleRequest( const string &ns, BSONObj query, int nToReturn, int nToSkip, BSONObj *fieldsToReturn, int queryOptions, Message &toSend ) {
                // see query.h for the protocol we are using here.
                BufBuilder b;
                int opts = queryOptions;
                assert( (opts&Option_ALLMASK) == opts );
                b.append(opts);
                b.append(ns.c_str());
                b.append(nToSkip);
                b.append(nToReturn);
                query.appendSelfToBufBuilder(b);
                if ( fieldsToReturn )
                    fieldsToReturn->appendSelfToBufBuilder(b);
                toSend.setData(dbQuery, b.buf(), b.len());
            }            
        protected:
            static const char *ns() { return "QueryPlanSetTests.coll"; }
            static NamespaceDetails *nsd() { return nsdetails( ns() ); }
        private:
            dblock lk_;
        };
        
        class NoIndexes : public Base {
        public:
            void run() {
                QueryPlanSet s( ns(), BSON( "a" << 4 ), BSON( "b" << 1 ) );
                ASSERT_EQUALS( 1, s.nPlans() );
            }
        };
        
        class Optimal : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), "a_1" );
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), "b_2" );
                QueryPlanSet s( ns(), BSON( "a" << 4 ), emptyObj );
                ASSERT_EQUALS( 2, s.nPlans() );                
            }
        };

        class NoOptimal : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), "a_1" );
                Helpers::ensureIndex( ns(), BSON( "b" << 1 ), "b_1" );
                QueryPlanSet s( ns(), BSON( "a" << 4 ), BSON( "b" << 1 ) );
                ASSERT_EQUALS( 3, s.nPlans() );
            }
        };

        class NoSpec : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), "a_1" );
                Helpers::ensureIndex( ns(), BSON( "b" << 1 ), "b_1" );
                QueryPlanSet s( ns(), emptyObj, emptyObj );
                ASSERT_EQUALS( 1, s.nPlans() );
            }
        };
        
        class HintSpec : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), "a_1" );
                Helpers::ensureIndex( ns(), BSON( "b" << 1 ), "b_1" );
                BSONObj b = BSON( "hint" << BSON( "a" << 1 ) );
                BSONElement e = b.firstElement();
                QueryPlanSet s( ns(), BSON( "a" << 1 ), BSON( "b" << 1 ), &e );
                ASSERT_EQUALS( 1, s.nPlans() );                
            }
        };

        class HintName : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), "a_1" );
                Helpers::ensureIndex( ns(), BSON( "b" << 1 ), "b_1" );
                BSONObj b = BSON( "hint" << "a_1" );
                BSONElement e = b.firstElement();
                QueryPlanSet s( ns(), BSON( "a" << 1 ), BSON( "b" << 1 ), &e );
                ASSERT_EQUALS( 1, s.nPlans() );                
            }
        };
        
        class NaturalHint : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), "a_1" );
                Helpers::ensureIndex( ns(), BSON( "b" << 1 ), "b_1" );
                BSONObj b = BSON( "hint" << BSON( "$natural" << 1 ) );
                BSONElement e = b.firstElement();
                QueryPlanSet s( ns(), BSON( "a" << 1 ), BSON( "b" << 1 ), &e );
                ASSERT_EQUALS( 1, s.nPlans() );                
            }
        };

        class NaturalSort : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), "a_1" );
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), "b_2" );
                QueryPlanSet s( ns(), BSON( "a" << 1 ), BSON( "$natural" << 1 ) );
                ASSERT_EQUALS( 1, s.nPlans() );
            }
        };

        class BadHint : public Base {
        public:
            void run() {
                BSONObj b = BSON( "hint" << "a_1" );
                BSONElement e = b.firstElement();
                ASSERT_EXCEPTION( QueryPlanSet s( ns(), BSON( "a" << 1 ), BSON( "b" << 1 ), &e ),
                                 AssertionException );
            }
        };
        
        class Count : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), "a_1" );
                Helpers::ensureIndex( ns(), BSON( "b" << 1 ), "b_1" );
                string err;
                ASSERT_EQUALS( 0, runCount( ns(), BSON( "query" << BSON( "a" << 4 ) ), err ) );
                BSONObj one = BSON( "a" << 1 );
                BSONObj four = BSON( "a" << 4 );
                theDataFileMgr.insert( ns(), one );
                ASSERT_EQUALS( 0, runCount( ns(), BSON( "query" << BSON( "a" << 4 ) ), err ) );
                theDataFileMgr.insert( ns(), four );
                ASSERT_EQUALS( 1, runCount( ns(), BSON( "query" << BSON( "a" << 4 ) ), err ) );
                theDataFileMgr.insert( ns(), four );
                ASSERT_EQUALS( 2, runCount( ns(), BSON( "query" << BSON( "a" << 4 ) ), err ) );
                ASSERT_EQUALS( 3, runCount( ns(), BSON( "query" << emptyObj ), err ) );
                ASSERT_EQUALS( 3, runCount( ns(), BSON( "query" << BSON( "a" << GT << 0 ) ), err ) );
                // missing ns
                ASSERT_EQUALS( -1, runCount( "missingNS", emptyObj, err ) );
                // impossible match
                ASSERT_EQUALS( 0, runCount( ns(), BSON( "query" << BSON( "a" << GT << 0 << LT << -1 ) ), err ) );
            }
        };
        
        class QueryMissingNs : public Base {
        public:
            void run() {
                Message m;
                assembleRequest( "missingNS", emptyObj, 0, 0, &emptyObj, 0, m );
                stringstream ss;
                ASSERT_EQUALS( 0, runQuery( m, ss )->nReturned );
            }
        };
        
        class UnhelpfulIndex : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), "a_1" );
                Helpers::ensureIndex( ns(), BSON( "b" << 1 ), "b_1" );
                QueryPlanSet s( ns(), BSON( "a" << 1 << "c" << 2 ), emptyObj );
                ASSERT_EQUALS( 2, s.nPlans() );                
            }
        };        
        
        class SingleException : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), "a_1" );
                Helpers::ensureIndex( ns(), BSON( "b" << 1 ), "b_1" );
                QueryPlanSet s( ns(), BSON( "a" << 4 ), BSON( "b" << 1 ) );
                ASSERT_EQUALS( 3, s.nPlans() );
                bool threw = false;
                auto_ptr< TestOp > t( new TestOp( true, threw ) );
                shared_ptr< TestOp > done = s.runOp( *t );
                ASSERT( threw );
                ASSERT( done->complete() );
                ASSERT( done->exceptionMessage().empty() );
                ASSERT( !done->error() );
            }
        private:
            class TestOp : public QueryOp {
            public:
                TestOp( bool iThrow, bool &threw ) : iThrow_( iThrow ), threw_( threw ), i_(), youThrow_( false ) {}
                virtual void init() {}
                virtual void next() {
                    if ( iThrow_ )
                        threw_ = true;
                    massert( "throw", !iThrow_ );
                    if ( ++i_ > 10 )
                        setComplete();
                }
                virtual QueryOp *clone() const {
                    QueryOp *op = new TestOp( youThrow_, threw_ );
                    youThrow_ = !youThrow_;
                    return op;
                }
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
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), "a_1" );
                Helpers::ensureIndex( ns(), BSON( "b" << 1 ), "b_1" );
                QueryPlanSet s( ns(), BSON( "a" << 4 ), BSON( "b" << 1 ) );
                ASSERT_EQUALS( 3, s.nPlans() );
                auto_ptr< TestOp > t( new TestOp() );
                shared_ptr< TestOp > done = s.runOp( *t );
                ASSERT( !done->complete() );
                ASSERT_EQUALS( "throw", done->exceptionMessage() );
                ASSERT( done->error() );
            }
        private:
            class TestOp : public QueryOp {
            public:
                virtual void init() {}
                virtual void next() {
                    massert( "throw", false );
                }
                virtual QueryOp *clone() const {
                    return new TestOp();
                }
            };
        };
        
        class SaveGoodIndex : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), "a_1" );
                Helpers::ensureIndex( ns(), BSON( "b" << 1 ), "b_1" );
                nPlans( 3 );
                runQuery();
                nPlans( 1 );
                nPlans( 1 );
                Helpers::ensureIndex( ns(), BSON( "c" << 1 ), "c_1" );
                nPlans( 3 );
                runQuery();
                nPlans( 1 );
                
                {
                    dbtemprelease t;
                    DBDirectClient client;
                    for( int i = 0; i < 34; ++i ) {
                        client.insert( ns(), BSON( "i" << i ) );
                        client.update( ns(), QUERY( "i" << i ), BSON( "i" << i + 1 ) );
                        client.remove( ns(), BSON( "i" << i + 1 ) );
                    }
                }
                nPlans( 3 );
                runQuery();
                nPlans( 1 );
            }
        private:
            void nPlans( int n ) {
                QueryPlanSet s( ns(), BSON( "a" << 4 ), BSON( "b" << 1 ) );
                ASSERT_EQUALS( n, s.nPlans() );                
            }
            void runQuery() {
                QueryPlanSet s( ns(), BSON( "a" << 4 ), BSON( "b" << 1 ) );
                TestOp original;
                s.runOp( original );
            }
            class TestOp : public QueryOp {
            public:
                virtual void init() {}
                virtual void next() {
                    setComplete();
                }
                virtual QueryOp *clone() const {
                    return new TestOp();
                }
            };            
        };        
        
    } // namespace QueryPlanSetTests
    
    class All : public UnitTest::Suite {
    public:
        All() {
            add< FieldBoundTests::Empty >();
            add< FieldBoundTests::Eq >();
            add< FieldBoundTests::DupEq >();
            add< FieldBoundTests::Lt >();
            add< FieldBoundTests::Lte >();
            add< FieldBoundTests::Gt >();
            add< FieldBoundTests::Gte >();
            add< FieldBoundTests::TwoLt >();
            add< FieldBoundTests::TwoGt >();
            add< FieldBoundTests::EqGte >();
            add< FieldBoundTests::EqGteInvalid >();
            add< FieldBoundTests::Regex >();
            add< FieldBoundTests::UnhelpfulRegex >();
            add< FieldBoundTests::In >();
            add< FieldBoundTests::SimplifiedQuery >();
            add< FieldBoundTests::QueryPatternTest >();
            add< QueryPlanTests::NoIndex >();
            add< QueryPlanTests::SimpleOrder >();
            add< QueryPlanTests::MoreIndexThanNeeded >();
            add< QueryPlanTests::IndexSigns >();
            add< QueryPlanTests::IndexReverse >();
            add< QueryPlanTests::NoOrder >();
            add< QueryPlanTests::EqualWithOrder >();
            add< QueryPlanTests::Optimal >();
            add< QueryPlanTests::MoreOptimal >();
            add< QueryPlanTests::KeyMatch >();
            add< QueryPlanTests::ExactKeyQueryTypes >();
            add< QueryPlanTests::Unhelpful >();
            add< QueryPlanSetTests::NoIndexes >();
            add< QueryPlanSetTests::Optimal >();
            add< QueryPlanSetTests::NoOptimal >();
            add< QueryPlanSetTests::NoSpec >();
            add< QueryPlanSetTests::HintSpec >();
            add< QueryPlanSetTests::HintName >();
            add< QueryPlanSetTests::NaturalHint >();
            add< QueryPlanSetTests::NaturalSort >();
            add< QueryPlanSetTests::BadHint >();
            add< QueryPlanSetTests::Count >();
            add< QueryPlanSetTests::QueryMissingNs >();
            add< QueryPlanSetTests::UnhelpfulIndex >();
            add< QueryPlanSetTests::SingleException >();
            add< QueryPlanSetTests::AllException >();
            add< QueryPlanSetTests::SaveGoodIndex >();
        }
    };
    
} // namespace QueryOptimizerTests

UnitTest::TestPtr queryOptimizerTests() {
    return UnitTest::createSuite< QueryOptimizerTests::All >();
}
