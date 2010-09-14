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
#include "../db/db.h"
#include "../db/dbhelpers.h"
#include "../db/instance.h"
#include "../db/query.h"
#include "dbtests.h"

namespace mongo {
    extern BSONObj id_obj;
    void runQuery(Message& m, QueryMessage& q, Message &response ){
        CurOp op( &(cc()) );
        op.ensureStarted();
        runQuery( m , q , op, response );
    }
    void runQuery(Message& m, QueryMessage& q ){
        Message response;
        runQuery( m, q, response );
    }
} // namespace mongo

namespace QueryOptimizerTests {

    namespace FieldRangeTests {
        class Base {
        public:
            virtual ~Base() {}
            void run() {
                const FieldRangeSet s( "ns", query() );
                checkElt( lower(), s.range( "a" ).min() );
                checkElt( upper(), s.range( "a" ).max() );
                ASSERT_EQUALS( lowerInclusive(), s.range( "a" ).minInclusive() );
                ASSERT_EQUALS( upperInclusive(), s.range( "a" ).maxInclusive() );
            }
        protected:
            virtual BSONObj query() = 0;
            virtual BSONElement lower() { return minKey.firstElement(); }
            virtual bool lowerInclusive() { return true; }
            virtual BSONElement upper() { return maxKey.firstElement(); }
            virtual bool upperInclusive() { return true; }
            static void checkElt( BSONElement expected, BSONElement actual ) {
                if ( expected.woCompare( actual, false ) ) {
                    log() << "expected: " << expected << ", got: " << actual;
                    ASSERT( false );
                }
            }
        };
        

        class NumericBase : public Base {
        public:
            NumericBase(){
                o = BSON( "min" << -numeric_limits<double>::max() << "max" << numeric_limits<double>::max() );
            }
            
            virtual BSONElement lower() { return o["min"]; }
            virtual BSONElement upper() { return o["max"]; }
        private:
            BSONObj o;
        };

        class Empty : public Base {
            virtual BSONObj query() { return BSONObj(); }
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

        class Lt : public NumericBase {
        public:
            Lt() : o_( BSON( "-" << 1 ) ) {}
            virtual BSONObj query() { return BSON( "a" << LT << 1 ); }
            virtual BSONElement upper() { return o_.firstElement(); }
            virtual bool upperInclusive() { return false; }
            BSONObj o_;
        };        

        class Lte : public Lt {
            virtual BSONObj query() { return BSON( "a" << LTE << 1 ); }            
            virtual bool upperInclusive() { return true; }
        };
        
        class Gt : public NumericBase {
        public:
            Gt() : o_( BSON( "-" << 1 ) ) {}
            virtual BSONObj query() { return BSON( "a" << GT << 1 ); }
            virtual BSONElement lower() { return o_.firstElement(); }
            virtual bool lowerInclusive() { return false; }
            BSONObj o_;
        };        
        
        class Gte : public Gt {
            virtual BSONObj query() { return BSON( "a" << GTE << 1 ); }            
            virtual bool lowerInclusive() { return true; }
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
                FieldRangeSet fbs( "ns", BSON( "a" << 1 << "a" << GTE << 2 ) );
                ASSERT( !fbs.matchPossible() );
            }
        };        

        struct RegexBase : Base {
            void run() { //need to only look at first interval
                FieldRangeSet s( "ns", query() );
                checkElt( lower(), s.range( "a" ).intervals()[0]._lower._bound );
                checkElt( upper(), s.range( "a" ).intervals()[0]._upper._bound );
                ASSERT_EQUALS( lowerInclusive(), s.range( "a" ).intervals()[0]._lower._inclusive );
                ASSERT_EQUALS( upperInclusive(), s.range( "a" ).intervals()[0]._upper._inclusive );
            }
        };

        class Regex : public RegexBase {
        public:
            Regex() : o1_( BSON( "" << "abc" ) ), o2_( BSON( "" << "abd" ) ) {}
            virtual BSONObj query() {
                BSONObjBuilder b;
                b.appendRegex( "a", "^abc" );
                return b.obj();
            }
            virtual BSONElement lower() { return o1_.firstElement(); }
            virtual BSONElement upper() { return o2_.firstElement(); }
            virtual bool upperInclusive() { return false; }
            BSONObj o1_, o2_;
        };        

        class RegexObj : public RegexBase {
        public:
            RegexObj() : o1_( BSON( "" << "abc" ) ), o2_( BSON( "" << "abd" ) ) {}
            virtual BSONObj query() { return BSON("a" << BSON("$regex" << "^abc")); }
            virtual BSONElement lower() { return o1_.firstElement(); }
            virtual BSONElement upper() { return o2_.firstElement(); }
            virtual bool upperInclusive() { return false; }
            BSONObj o1_, o2_;
        };
        
        class UnhelpfulRegex : public RegexBase {
        public:
            UnhelpfulRegex() {
                BSONObjBuilder b;
                b.appendMinForType("lower", String);
                b.appendMaxForType("upper", String);
                limits = b.obj();
            }

            virtual BSONObj query() {
                BSONObjBuilder b;
                b.appendRegex( "a", "abc" );
                return b.obj();
            }            
            virtual BSONElement lower() { return limits["lower"]; }
            virtual BSONElement upper() { return limits["upper"]; }
            virtual bool upperInclusive() { return false; }
            BSONObj limits;
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
        
        class Equality {
        public:
            void run() {
                FieldRangeSet s( "ns", BSON( "a" << 1 ) );
                ASSERT( s.range( "a" ).equality() );
                FieldRangeSet s2( "ns", BSON( "a" << GTE << 1 << LTE << 1 ) );
                ASSERT( s2.range( "a" ).equality() );
                FieldRangeSet s3( "ns", BSON( "a" << GT << 1 << LTE << 1 ) );
                ASSERT( !s3.range( "a" ).equality() );
                FieldRangeSet s4( "ns", BSON( "a" << GTE << 1 << LT << 1 ) );
                ASSERT( !s4.range( "a" ).equality() );
                FieldRangeSet s5( "ns", BSON( "a" << GTE << 1 << LTE << 1 << GT << 1 ) );
                ASSERT( !s5.range( "a" ).equality() );
                FieldRangeSet s6( "ns", BSON( "a" << GTE << 1 << LTE << 1 << LT << 1 ) );
                ASSERT( !s6.range( "a" ).equality() );
            }
        };
        
        class SimplifiedQuery {
        public:
            void run() {
                FieldRangeSet fbs( "ns", BSON( "a" << GT << 1 << GT << 5 << LT << 10 << "b" << 4 << "c" << LT << 4 << LT << 6 << "d" << GTE << 0 << GT << 0 << "e" << GTE << 0 << LTE << 10 ) );
                BSONObj simple = fbs.simplifiedQuery();
                cout << "simple: " << simple << endl;
                ASSERT( !simple.getObjectField( "a" ).woCompare( fromjson( "{$gt:5,$lt:10}" ) ) );
                ASSERT_EQUALS( 4, simple.getIntField( "b" ) );
                ASSERT( !simple.getObjectField( "c" ).woCompare( BSON("$gte" << -numeric_limits<double>::max() << "$lt" << 4 ) ) );
                ASSERT( !simple.getObjectField( "d" ).woCompare( BSON("$gt" << 0 << "$lte" << numeric_limits<double>::max() ) ) );
                ASSERT( !simple.getObjectField( "e" ).woCompare( fromjson( "{$gte:0,$lte:10}" ) ) );
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
                ASSERT( p( BSON( "a" << 1 ) ) < p( BSON( "a" << 1 << "b" << 1 ) ) );
                ASSERT( !( p( BSON( "a" << 1 << "b" << 1 ) ) < p( BSON( "a" << 1 ) ) ) );
                ASSERT( p( BSON( "a" << 1 ), BSON( "b" << 1 ) ) == p( BSON( "a" << 4 ), BSON( "b" << "a" ) ) );
                ASSERT( p( BSON( "a" << 1 ), BSON( "b" << 1 ) ) == p( BSON( "a" << 4 ), BSON( "b" << -1 ) ) );
                ASSERT( p( BSON( "a" << 1 ), BSON( "b" << 1 ) ) != p( BSON( "a" << 4 ), BSON( "c" << 1 ) ) );
                ASSERT( p( BSON( "a" << 1 ), BSON( "b" << 1 << "c" << -1 ) ) == p( BSON( "a" << 4 ), BSON( "b" << -1 << "c" << 1 ) ) );
                ASSERT( p( BSON( "a" << 1 ), BSON( "b" << 1 << "c" << 1 ) ) != p( BSON( "a" << 4 ), BSON( "b" << 1 ) ) );
                ASSERT( p( BSON( "a" << 1 ), BSON( "b" << 1 ) ) != p( BSON( "a" << 4 ), BSON( "b" << 1 << "c" << 1 ) ) );
            }
        private:
            static QueryPattern p( const BSONObj &query, const BSONObj &sort = BSONObj() ) {
                return FieldRangeSet( "", query ).pattern( sort );
            }
        };
        
        class NoWhere {
        public:
            void run() {
                ASSERT_EQUALS( 0, FieldRangeSet( "ns", BSON( "$where" << 1 ) ).nNontrivialRanges() );
            }
        };
        
        class Numeric {
        public:
            void run() {
                FieldRangeSet f( "", BSON( "a" << 1 ) );
                ASSERT( f.range( "a" ).min().woCompare( BSON( "a" << 2.0 ).firstElement() ) < 0 );
                ASSERT( f.range( "a" ).min().woCompare( BSON( "a" << 0.0 ).firstElement() ) > 0 );
            }
        };

        class InLowerBound {
        public:
            void run() {
                FieldRangeSet f( "", fromjson( "{a:{$gt:4,$in:[1,2,3,4,5,6]}}" ) );
                ASSERT( f.range( "a" ).min().woCompare( BSON( "a" << 5.0 ).firstElement(), false ) == 0 );
                ASSERT( f.range( "a" ).max().woCompare( BSON( "a" << 6.0 ).firstElement(), false ) == 0 );
            }
        };

        class InUpperBound {
        public:
            void run() {
                FieldRangeSet f( "", fromjson( "{a:{$lt:4,$in:[1,2,3,4,5,6]}}" ) );
                ASSERT( f.range( "a" ).min().woCompare( BSON( "a" << 1.0 ).firstElement(), false ) == 0 );
                ASSERT( f.range( "a" ).max().woCompare( BSON( "a" << 3.0 ).firstElement(), false ) == 0 );
            }
        };
		
        class UnionBound {
        public:
            void run() {
                FieldRangeSet frs( "", fromjson( "{a:{$gt:1,$lt:9},b:{$gt:9,$lt:12}}" ) );
                FieldRange ret = frs.range( "a" );
                ret |= frs.range( "b" );
                ASSERT_EQUALS( 2U, ret.intervals().size() );
            }
        };
        
		class MultiBound {
		public:
			void run() {
                FieldRangeSet frs1( "", fromjson( "{a:{$in:[1,3,5,7,9]}}" ) );
                FieldRangeSet frs2( "", fromjson( "{a:{$in:[2,3,5,8,9]}}" ) );
				FieldRange fr1 = frs1.range( "a" );
				FieldRange fr2 = frs2.range( "a" );
				fr1 &= fr2;
                ASSERT( fr1.min().woCompare( BSON( "a" << 3.0 ).firstElement(), false ) == 0 );
                ASSERT( fr1.max().woCompare( BSON( "a" << 9.0 ).firstElement(), false ) == 0 );
				vector< FieldInterval > intervals = fr1.intervals();
				vector< FieldInterval >::const_iterator j = intervals.begin();
				double expected[] = { 3, 5, 9 };
				for( int i = 0; i < 3; ++i, ++j ) {
					ASSERT_EQUALS( expected[ i ], j->_lower._bound.number() );
					ASSERT( j->_lower._inclusive );
					ASSERT( j->_lower == j->_upper );
				}
				ASSERT( j == intervals.end() );
			}
		};
        
        class DiffBase {
        public:
            virtual ~DiffBase() {}
            void run() {
                FieldRangeSet frs( "", fromjson( obj().toString() ) );
                FieldRange ret = frs.range( "a" );
                ret -= frs.range( "b" );
                check( ret );                
            }
        protected:
            void check( const FieldRange &fr ) {
                vector< FieldInterval > fi = fr.intervals();
                ASSERT_EQUALS( len(), fi.size() );
                int i = 0;
                for( vector< FieldInterval >::const_iterator j = fi.begin(); j != fi.end(); ++j ) {
                    ASSERT_EQUALS( nums()[ i ], j->_lower._bound.numberInt() );
                    ASSERT_EQUALS( incs()[ i ], j->_lower._inclusive );
                    ++i;
                    ASSERT_EQUALS( nums()[ i ], j->_upper._bound.numberInt() );
                    ASSERT_EQUALS( incs()[ i ], j->_upper._inclusive );
                    ++i;
                }
            }
            virtual unsigned len() const = 0;
            virtual const int *nums() const = 0;
            virtual const bool *incs() const = 0;
            virtual BSONObj obj() const = 0;
        };

        class TwoRangeBase : public DiffBase {
        public:
            TwoRangeBase( string obj, int low, int high, bool lowI, bool highI )
            : _obj( obj ) {
                _n[ 0 ] = low;
                _n[ 1 ] = high;
                _b[ 0 ] = lowI;
                _b[ 1 ] = highI;
            }
        private:
            virtual unsigned len() const { return 1; }
            virtual const int *nums() const { return _n; }
            virtual const bool *incs() const { return _b; }
            virtual BSONObj obj() const { return fromjson( _obj ); }
            string _obj;
            int _n[ 2 ];
            bool _b[ 2 ];
        };
        
        struct Diff1 : public TwoRangeBase {
            Diff1() : TwoRangeBase( "{a:{$gt:1,$lt:2},b:{$gt:3,$lt:4}}", 1, 2, false, false ) {}
        };

        struct Diff2 : public TwoRangeBase {
            Diff2() : TwoRangeBase( "{a:{$gt:1,$lt:2},b:{$gt:2,$lt:4}}", 1, 2, false, false ) {}
        };
        
        struct Diff3 : public TwoRangeBase {
            Diff3() : TwoRangeBase( "{a:{$gt:1,$lte:2},b:{$gt:2,$lt:4}}", 1, 2, false, true ) {}
        };

        struct Diff4 : public TwoRangeBase {
            Diff4() : TwoRangeBase( "{a:{$gt:1,$lt:2},b:{$gte:2,$lt:4}}", 1, 2, false, false) {}
        };
        
        struct Diff5 : public TwoRangeBase {
            Diff5() : TwoRangeBase( "{a:{$gt:1,$lte:2},b:{$gte:2,$lt:4}}", 1, 2, false, false) {}
        };
        
        struct Diff6 : public TwoRangeBase {
            Diff6() : TwoRangeBase( "{a:{$gt:1,$lte:3},b:{$gte:2,$lt:4}}", 1, 2, false, false) {}
        };

        struct Diff7 : public TwoRangeBase {
            Diff7() : TwoRangeBase( "{a:{$gt:1,$lte:3},b:{$gt:2,$lt:4}}", 1, 2, false, true) {}
        };
        
        struct Diff8 : public TwoRangeBase {
            Diff8() : TwoRangeBase( "{a:{$gt:1,$lt:4},b:{$gt:2,$lt:4}}", 1, 2, false, true) {}
        };

        struct Diff9 : public TwoRangeBase {
            Diff9() : TwoRangeBase( "{a:{$gt:1,$lt:4},b:{$gt:2,$lte:4}}", 1, 2, false, true) {}
        };

        struct Diff10 : public TwoRangeBase {
            Diff10() : TwoRangeBase( "{a:{$gt:1,$lte:4},b:{$gt:2,$lte:4}}", 1, 2, false, true) {}
        };        
        
        class SplitRangeBase : public DiffBase {
        public:
            SplitRangeBase( string obj, int low1, bool low1I, int high1, bool high1I, int low2, bool low2I, int high2, bool high2I )
            : _obj( obj ) {
                _n[ 0 ] = low1;
                _n[ 1 ] = high1;
                _n[ 2 ] = low2;
                _n[ 3 ] = high2;
                _b[ 0 ] = low1I;
                _b[ 1 ] = high1I;
                _b[ 2 ] = low2I;
                _b[ 3 ] = high2I;
            }
        private:
            virtual unsigned len() const { return 2; }
            virtual const int *nums() const { return _n; }
            virtual const bool *incs() const { return _b; }
            virtual BSONObj obj() const { return fromjson( _obj ); }
            string _obj;
            int _n[ 4 ];
            bool _b[ 4 ];            
        };
        
        struct Diff11 : public SplitRangeBase {
            Diff11() : SplitRangeBase( "{a:{$gt:1,$lte:4},b:{$gt:2,$lt:4}}", 1, false, 2, true, 4, true, 4, true) {}
        };

        struct Diff12 : public SplitRangeBase {
            Diff12() : SplitRangeBase( "{a:{$gt:1,$lt:5},b:{$gt:2,$lt:4}}", 1, false, 2, true, 4, true, 5, false) {}
        };
        
        struct Diff13 : public TwoRangeBase {
            Diff13() : TwoRangeBase( "{a:{$gt:1,$lt:5},b:{$gt:1,$lt:4}}", 4, 5, true, false) {}
        };
        
        struct Diff14 : public SplitRangeBase {
            Diff14() : SplitRangeBase( "{a:{$gte:1,$lt:5},b:{$gt:1,$lt:4}}", 1, true, 1, true, 4, true, 5, false) {}
        };

        struct Diff15 : public TwoRangeBase {
            Diff15() : TwoRangeBase( "{a:{$gt:1,$lt:5},b:{$gte:1,$lt:4}}", 4, 5, true, false) {}
        };

        struct Diff16 : public TwoRangeBase {
            Diff16() : TwoRangeBase( "{a:{$gte:1,$lt:5},b:{$gte:1,$lt:4}}", 4, 5, true, false) {}
        };

        struct Diff17 : public TwoRangeBase {
            Diff17() : TwoRangeBase( "{a:{$gt:1,$lt:5},b:{$gt:0,$lt:4}}", 4, 5, true, false) {}
        };

        struct Diff18 : public TwoRangeBase {
            Diff18() : TwoRangeBase( "{a:{$gt:1,$lt:5},b:{$gt:0,$lte:4}}", 4, 5, false, false) {}
        };

        struct Diff19 : public TwoRangeBase {
            Diff19() : TwoRangeBase( "{a:{$gte:1,$lte:5},b:{$gte:0,$lte:1}}", 1, 5, false, true) {}
        };

        struct Diff20 : public TwoRangeBase {
            Diff20() : TwoRangeBase( "{a:{$gt:1,$lte:5},b:{$gte:0,$lte:1}}", 1, 5, false, true) {}
        };

        struct Diff21 : public TwoRangeBase {
            Diff21() : TwoRangeBase( "{a:{$gte:1,$lte:5},b:{$gte:0,$lt:1}}", 1, 5, true, true) {}
        };

        struct Diff22 : public TwoRangeBase {
            Diff22() : TwoRangeBase( "{a:{$gt:1,$lte:5},b:{$gte:0,$lt:1}}", 1, 5, false, true) {}
        };

        struct Diff23 : public TwoRangeBase {
            Diff23() : TwoRangeBase( "{a:{$gt:1,$lte:5},b:{$gte:0,$lt:0.5}}", 1, 5, false, true) {}
        };

        struct Diff24 : public TwoRangeBase {
            Diff24() : TwoRangeBase( "{a:{$gt:1,$lte:5},b:0}", 1, 5, false, true) {}
        };

        struct Diff25 : public TwoRangeBase {
            Diff25() : TwoRangeBase( "{a:{$gte:1,$lte:5},b:0}", 1, 5, true, true) {}
        };
        
        struct Diff26 : public TwoRangeBase {
            Diff26() : TwoRangeBase( "{a:{$gt:1,$lte:5},b:1}", 1, 5, false, true) {}
        };

        struct Diff27 : public TwoRangeBase {
            Diff27() : TwoRangeBase( "{a:{$gte:1,$lte:5},b:1}", 1, 5, false, true) {}
        };

        struct Diff28 : public SplitRangeBase {
            Diff28() : SplitRangeBase( "{a:{$gte:1,$lte:5},b:3}", 1, true, 3, false, 3, false, 5, true) {}
        };

        struct Diff29 : public TwoRangeBase {
            Diff29() : TwoRangeBase( "{a:{$gte:1,$lte:5},b:5}", 1, 5, true, false) {}
        };
        
        struct Diff30 : public TwoRangeBase {
            Diff30() : TwoRangeBase( "{a:{$gte:1,$lt:5},b:5}", 1, 5, true, false) {}
        };

        struct Diff31 : public TwoRangeBase {
            Diff31() : TwoRangeBase( "{a:{$gte:1,$lt:5},b:6}", 1, 5, true, false) {}
        };
        
        struct Diff32 : public TwoRangeBase {
            Diff32() : TwoRangeBase( "{a:{$gte:1,$lte:5},b:6}", 1, 5, true, true) {}
        };

        class EmptyBase : public DiffBase {
        public:
            EmptyBase( string obj )
            : _obj( obj ) {}
        private:
            virtual unsigned len() const { return 0; }
            virtual const int *nums() const { return 0; }
            virtual const bool *incs() const { return 0; }
            virtual BSONObj obj() const { return fromjson( _obj ); }
            string _obj;
        };
                
        struct Diff33 : public EmptyBase {
            Diff33() : EmptyBase( "{a:{$gte:1,$lte:5},b:{$gt:0,$lt:6}}" ) {}
        };

        struct Diff34 : public EmptyBase {
            Diff34() : EmptyBase( "{a:{$gte:1,$lte:5},b:{$gte:1,$lt:6}}" ) {}
        };

        struct Diff35 : public EmptyBase {
            Diff35() : EmptyBase( "{a:{$gt:1,$lte:5},b:{$gte:1,$lt:6}}" ) {}
        };

        struct Diff36 : public EmptyBase {
            Diff36() : EmptyBase( "{a:{$gt:1,$lte:5},b:{$gt:1,$lt:6}}" ) {}
        };

        struct Diff37 : public TwoRangeBase {
            Diff37() : TwoRangeBase( "{a:{$gte:1,$lte:5},b:{$gt:1,$lt:6}}", 1, 1, true, true ) {}
        };

        struct Diff38 : public EmptyBase {
            Diff38() : EmptyBase( "{a:{$gt:1,$lt:5},b:{$gt:0,$lt:5}}" ) {}
        };

        struct Diff39 : public EmptyBase {
            Diff39() : EmptyBase( "{a:{$gt:1,$lt:5},b:{$gt:0,$lte:5}}" ) {}
        };

        struct Diff40 : public EmptyBase {
            Diff40() : EmptyBase( "{a:{$gt:1,$lte:5},b:{$gt:0,$lte:5}}" ) {}
        };
        
        struct Diff41 : public TwoRangeBase {
            Diff41() : TwoRangeBase( "{a:{$gte:1,$lte:5},b:{$gt:0,$lt:5}}", 5, 5, true, true ) {}
        };

        struct Diff42 : public EmptyBase {
            Diff42() : EmptyBase( "{a:{$gt:1,$lt:5},b:{$gt:1,$lt:5}}" ) {}
        };

        struct Diff43 : public EmptyBase {
            Diff43() : EmptyBase( "{a:{$gt:1,$lt:5},b:{$gt:1,$lte:5}}" ) {}
        };

        struct Diff44 : public EmptyBase {
            Diff44() : EmptyBase( "{a:{$gt:1,$lt:5},b:{$gte:1,$lt:5}}" ) {}
        };

        struct Diff45 : public EmptyBase {
            Diff45() : EmptyBase( "{a:{$gt:1,$lt:5},b:{$gte:1,$lte:5}}" ) {}
        };

        struct Diff46 : public TwoRangeBase {
            Diff46() : TwoRangeBase( "{a:{$gt:1,$lte:5},b:{$gt:1,$lt:5}}", 5, 5, true, true ) {}
        };

        struct Diff47 : public EmptyBase {
            Diff47() : EmptyBase( "{a:{$gt:1,$lte:5},b:{$gt:1,$lte:5}}" ) {}
        };

        struct Diff48 : public TwoRangeBase {
            Diff48() : TwoRangeBase( "{a:{$gt:1,$lte:5},b:{$gte:1,$lt:5}}", 5, 5, true, true ) {}
        };

        struct Diff49 : public EmptyBase {
            Diff49() : EmptyBase( "{a:{$gt:1,$lte:5},b:{$gte:1,$lte:5}}" ) {}
        };

        struct Diff50 : public TwoRangeBase {
            Diff50() : TwoRangeBase( "{a:{$gte:1,$lt:5},b:{$gt:1,$lt:5}}", 1, 1, true, true ) {}
        };

        struct Diff51 : public TwoRangeBase {
            Diff51() : TwoRangeBase( "{a:{$gte:1,$lt:5},b:{$gt:1,$lte:5}}", 1, 1, true, true ) {}
        };

        struct Diff52 : public EmptyBase {
            Diff52() : EmptyBase( "{a:{$gte:1,$lt:5},b:{$gte:1,$lt:5}}" ) {}
        };

        struct Diff53 : public EmptyBase {
            Diff53() : EmptyBase( "{a:{$gte:1,$lt:5},b:{$gte:1,$lte:5}}" ) {}
        };

        struct Diff54 : public SplitRangeBase {
            Diff54() : SplitRangeBase( "{a:{$gte:1,$lte:5},b:{$gt:1,$lt:5}}", 1, true, 1, true, 5, true, 5, true ) {}
        };

        struct Diff55 : public TwoRangeBase {
            Diff55() : TwoRangeBase( "{a:{$gte:1,$lte:5},b:{$gt:1,$lte:5}}", 1, 1, true, true ) {}
        };

        struct Diff56 : public TwoRangeBase {
            Diff56() : TwoRangeBase( "{a:{$gte:1,$lte:5},b:{$gte:1,$lt:5}}", 5, 5, true, true ) {}
        };

        struct Diff57 : public EmptyBase {
            Diff57() : EmptyBase( "{a:{$gte:1,$lte:5},b:{$gte:1,$lte:5}}" ) {}
        };
        
        struct Diff58 : public TwoRangeBase {
            Diff58() : TwoRangeBase( "{a:1,b:{$gt:1,$lt:5}}", 1, 1, true, true ) {}
        };

        struct Diff59 : public EmptyBase {
            Diff59() : EmptyBase( "{a:1,b:{$gte:1,$lt:5}}" ) {}
        };

        struct Diff60 : public EmptyBase {
            Diff60() : EmptyBase( "{a:2,b:{$gte:1,$lt:5}}" ) {}
        };

        struct Diff61 : public EmptyBase {
            Diff61() : EmptyBase( "{a:5,b:{$gte:1,$lte:5}}" ) {}
        };

        struct Diff62 : public TwoRangeBase {
            Diff62() : TwoRangeBase( "{a:5,b:{$gt:1,$lt:5}}", 5, 5, true, true ) {}
        };

        struct Diff63 : public EmptyBase {
            Diff63() : EmptyBase( "{a:5,b:5}" ) {}
        };
        
        struct Diff64 : public TwoRangeBase {
            Diff64() : TwoRangeBase( "{a:{$gte:1,$lte:2},b:{$gt:0,$lte:1}}", 1, 2, false, true ) {}
        };
        
        class DiffMulti1 : public DiffBase {
        public:
            void run() {
                FieldRangeSet frs( "", fromjson( "{a:{$gt:1,$lt:9},b:{$gt:0,$lt:2},c:3,d:{$gt:4,$lt:5},e:{$gt:7,$lt:10}}" ) );
                FieldRange ret = frs.range( "a" );
                FieldRange other = frs.range( "b" );
                other |= frs.range( "c" );
                other |= frs.range( "d" );
                other |= frs.range( "e" );
                ret -= other;
                check( ret );                
            }
        protected:
            virtual unsigned len() const { return 3; }
            virtual const int *nums() const { static int n[] = { 2, 3, 3, 4, 5, 7 }; return n; }
            virtual const bool *incs() const { static bool b[] = { true, false, false, true, true, true }; return b; }
            virtual BSONObj obj() const { return BSONObj(); }
        };

        class DiffMulti2 : public DiffBase {
        public:
            void run() {
                FieldRangeSet frs( "", fromjson( "{a:{$gt:1,$lt:9},b:{$gt:0,$lt:2},c:3,d:{$gt:4,$lt:5},e:{$gt:7,$lt:10}}" ) );
                FieldRange mask = frs.range( "a" );
                FieldRange ret = frs.range( "b" );
                ret |= frs.range( "c" );
                ret |= frs.range( "d" );
                ret |= frs.range( "e" );
                ret -= mask;
                check( ret );                
            }
        protected:
            virtual unsigned len() const { return 2; }
            virtual const int *nums() const { static int n[] = { 0, 1, 9, 10 }; return n; }
            virtual const bool *incs() const { static bool b[] = { false, true, true, false }; return b; }
            virtual BSONObj obj() const { return BSONObj(); }
        };
        
        class SetIntersect {
        public:
            void run() {
                FieldRangeSet frs1( "", fromjson( "{b:{$in:[5,6]},c:7,d:{$in:[8,9]}}" ) );
                FieldRangeSet frs2( "", fromjson( "{a:1,b:5,c:{$in:[7,8]},d:{$in:[8,9]},e:10}" ) );
                frs1 &= frs2;
                ASSERT_EQUALS( fromjson( "{a:1,b:5,c:7,d:{$gte:8,$lte:9},e:10}" ), frs1.simplifiedQuery( BSONObj() ) );
            }
        };
        
    } // namespace FieldRangeTests
    
    namespace QueryPlanTests {
        class Base {
        public:
            Base() : _ctx( ns() ) , indexNum_( 0 ) {
                string err;
                userCreateNS( ns(), BSONObj(), err, false );
            }
            ~Base() {
                if ( !nsd() )
                    return;
                string s( ns() );
                dropNS( s );
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
        auto_ptr< FieldRangeSet > FieldRangeSet_GLOBAL;
#define FBS(x) ( FieldRangeSet_GLOBAL.reset( new FieldRangeSet( ns(), x ) ), *FieldRangeSet_GLOBAL )
        auto_ptr< FieldRangeSet > FieldRangeSet_GLOBAL2;
#define FBS2(x) ( FieldRangeSet_GLOBAL2.reset( new FieldRangeSet( ns(), x ) ), *FieldRangeSet_GLOBAL2 )
        
        class NoIndex : public Base {
        public:
            void run() {
                QueryPlan p( nsd(), -1, FBS( BSONObj() ), FBS2( BSONObj() ), BSONObj(), BSONObj() );
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
                
                QueryPlan p( nsd(), INDEXNO( "a" << 1 ), FBS( BSONObj() ), FBS2( BSONObj() ), BSONObj(), BSON( "a" << 1 ) );
                ASSERT( !p.scanAndOrderRequired() );
                ASSERT( !startKey( p ).woCompare( start ) );
                ASSERT( !endKey( p ).woCompare( end ) );
                QueryPlan p2( nsd(), INDEXNO( "a" << 1 << "b" << 1 ), FBS( BSONObj() ), FBS2( BSONObj() ), BSONObj(), BSON( "a" << 1 << "b" << 1 ) );
                ASSERT( !p2.scanAndOrderRequired() );
                QueryPlan p3( nsd(), INDEXNO( "a" << 1 ), FBS( BSONObj() ), FBS2( BSONObj() ), BSONObj(), BSON( "b" << 1 ) );
                ASSERT( p3.scanAndOrderRequired() );
                ASSERT( !startKey( p3 ).woCompare( start ) );
                ASSERT( !endKey( p3 ).woCompare( end ) );
            }
        };
        
        class MoreIndexThanNeeded : public Base {
        public:
            void run() {
                QueryPlan p( nsd(), INDEXNO( "a" << 1 << "b" << 1 ), FBS( BSONObj() ), FBS2( BSONObj() ), BSONObj(), BSON( "a" << 1 ) );
                ASSERT( !p.scanAndOrderRequired() );                
            }
        };
        
        class IndexSigns : public Base {
        public:
            void run() {
                QueryPlan p( nsd(), INDEXNO( "a" << 1 << "b" << -1 ) , FBS( BSONObj() ), FBS2( BSONObj() ), BSONObj(), BSON( "a" << 1 << "b" << -1 ) );
                ASSERT( !p.scanAndOrderRequired() );                
                ASSERT_EQUALS( 1, p.direction() );
                QueryPlan p2( nsd(), INDEXNO( "a" << 1 << "b" << 1 ), FBS( BSONObj() ), FBS2( BSONObj() ), BSONObj(), BSON( "a" << 1 << "b" << -1 ) );
                ASSERT( p2.scanAndOrderRequired() );                
                ASSERT_EQUALS( 0, p2.direction() );
                QueryPlan p3( nsd(), indexno( id_obj ), FBS( BSONObj() ), FBS2( BSONObj() ), BSONObj(), BSON( "_id" << 1 ) );
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
                QueryPlan p( nsd(),  INDEXNO( "a" << -1 << "b" << 1 ),FBS( BSONObj() ), FBS2( BSONObj() ), BSONObj(), BSON( "a" << 1 << "b" << -1 ) );
                ASSERT( !p.scanAndOrderRequired() );                
                ASSERT_EQUALS( -1, p.direction() );
                ASSERT( !startKey( p ).woCompare( start ) );
                ASSERT( !endKey( p ).woCompare( end ) );
                QueryPlan p2( nsd(), INDEXNO( "a" << 1 << "b" << 1 ), FBS( BSONObj() ), FBS2( BSONObj() ), BSONObj(), BSON( "a" << -1 << "b" << -1 ) );
                ASSERT( !p2.scanAndOrderRequired() );                
                ASSERT_EQUALS( -1, p2.direction() );
                QueryPlan p3( nsd(), INDEXNO( "a" << 1 << "b" << -1 ), FBS( BSONObj() ), FBS2( BSONObj() ), BSONObj(), BSON( "a" << -1 << "b" << -1 ) );
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
                QueryPlan p( nsd(), INDEXNO( "a" << -1 << "b" << 1 ), FBS( BSON( "a" << 3 ) ), FBS2( BSON( "a" << 3 ) ), BSON( "a" << 3 ), BSONObj() );
                ASSERT( !p.scanAndOrderRequired() );                
                ASSERT( !startKey( p ).woCompare( start ) );
                ASSERT( !endKey( p ).woCompare( end ) );
                QueryPlan p2( nsd(), INDEXNO( "a" << -1 << "b" << 1 ), FBS( BSON( "a" << 3 ) ), FBS2( BSON( "a" << 3 ) ), BSON( "a" << 3 ), BSONObj() );
                ASSERT( !p2.scanAndOrderRequired() );                
                ASSERT( !startKey( p ).woCompare( start ) );
                ASSERT( !endKey( p ).woCompare( end ) );
            }            
        };
        
        class EqualWithOrder : public Base {
        public:
            void run() {
                QueryPlan p( nsd(), INDEXNO( "a" << 1 << "b" << 1 ), FBS( BSON( "a" << 4 ) ), FBS2( BSON( "a" << 4 ) ), BSON( "a" << 4 ), BSON( "b" << 1 ) );
                ASSERT( !p.scanAndOrderRequired() );                
                QueryPlan p2( nsd(), INDEXNO( "a" << 1 << "b" << 1 << "c" << 1 ), FBS( BSON( "b" << 4 ) ), FBS2( BSON( "b" << 4 ) ), BSON( "b" << 4 ), BSON( "a" << 1 << "c" << 1 ) );
                ASSERT( !p2.scanAndOrderRequired() );                
                QueryPlan p3( nsd(), INDEXNO( "a" << 1 << "b" << 1 ), FBS( BSON( "b" << 4 ) ), FBS2( BSON( "b" << 4 ) ), BSON( "b" << 4 ), BSON( "a" << 1 << "c" << 1 ) );
                ASSERT( p3.scanAndOrderRequired() );                
            }
        };
        
        class Optimal : public Base {
        public:
            void run() {
                QueryPlan p( nsd(), INDEXNO( "a" << 1 ), FBS( BSONObj() ), FBS2( BSONObj() ), BSONObj(), BSON( "a" << 1 ) );
                ASSERT( p.optimal() );
                QueryPlan p2( nsd(), INDEXNO( "a" << 1 << "b" << 1 ), FBS( BSONObj() ), FBS2( BSONObj() ), BSONObj(), BSON( "a" << 1 ) );
                ASSERT( p2.optimal() );
                QueryPlan p3( nsd(), INDEXNO( "a" << 1 << "b" << 1 ), FBS( BSON( "a" << 1 ) ), FBS2( BSON( "a" << 1 ) ), BSON( "a" << 1 ), BSON( "a" << 1 ) );
                ASSERT( p3.optimal() );
                QueryPlan p4( nsd(), INDEXNO( "a" << 1 << "b" << 1 ), FBS( BSON( "b" << 1 ) ), FBS2( BSON( "b" << 1 ) ), BSON( "b" << 1 ), BSON( "a" << 1 ) );
                ASSERT( !p4.optimal() );
                QueryPlan p5( nsd(), INDEXNO( "a" << 1 << "b" << 1 ), FBS( BSON( "a" << 1 ) ), FBS2( BSON( "a" << 1 ) ), BSON( "a" << 1 ), BSON( "b" << 1 ) );
                ASSERT( p5.optimal() );
                QueryPlan p6( nsd(), INDEXNO( "a" << 1 << "b" << 1 ), FBS( BSON( "b" << 1 ) ), FBS2( BSON( "b" << 1 ) ), BSON( "b" << 1 ), BSON( "b" << 1 ) );
                ASSERT( !p6.optimal() );
                QueryPlan p7( nsd(), INDEXNO( "a" << 1 << "b" << 1 ), FBS( BSON( "a" << 1 << "b" << 1 ) ), FBS2( BSON( "a" << 1 << "b" << 1 ) ), BSON( "a" << 1 << "b" << 1 ), BSON( "a" << 1 ) );
                ASSERT( p7.optimal() );
                QueryPlan p8( nsd(), INDEXNO( "a" << 1 << "b" << 1 ), FBS( BSON( "a" << 1 << "b" << LT << 1 ) ), FBS2( BSON( "a" << 1 << "b" << LT << 1 ) ), BSON( "a" << 1 << "b" << LT << 1 ), BSON( "a" << 1 )  );
                ASSERT( p8.optimal() );
                QueryPlan p9( nsd(), INDEXNO( "a" << 1 << "b" << 1 << "c" << 1 ), FBS( BSON( "a" << 1 << "b" << LT << 1 ) ), FBS2( BSON( "a" << 1 << "b" << LT << 1 ) ), BSON( "a" << 1 << "b" << LT << 1 ), BSON( "a" << 1 ) );
                ASSERT( p9.optimal() );
            }
        };
        
        class MoreOptimal : public Base {
        public:
            void run() {
                 QueryPlan p10( nsd(), INDEXNO( "a" << 1 << "b" << 1 << "c" << 1 ), FBS( BSON( "a" << 1 ) ), FBS2( BSON( "a" << 1 ) ), BSON( "a" << 1 ), BSONObj() );
                 ASSERT( p10.optimal() );
                 QueryPlan p11( nsd(), INDEXNO( "a" << 1 << "b" << 1 << "c" << 1 ), FBS( BSON( "a" << 1 << "b" << LT << 1 ) ), FBS2( BSON( "a" << 1 << "b" << LT << 1 ) ), BSON( "a" << 1 << "b" << LT << 1 ), BSONObj() );
                 ASSERT( p11.optimal() );
                 QueryPlan p12( nsd(), INDEXNO( "a" << 1 << "b" << 1 << "c" << 1 ), FBS( BSON( "a" << LT << 1 ) ), FBS2( BSON( "a" << LT << 1 ) ), BSON( "a" << LT << 1 ), BSONObj() );
                 ASSERT( p12.optimal() );
                 QueryPlan p13( nsd(), INDEXNO( "a" << 1 << "b" << 1 << "c" << 1 ), FBS( BSON( "a" << LT << 1 ) ), FBS2( BSON( "a" << LT << 1 ) ), BSON( "a" << LT << 1 ), BSON( "a" << 1 ) );
                 ASSERT( p13.optimal() );
            }
        };
        
        class KeyMatch : public Base {
        public:
            void run() {
                QueryPlan p( nsd(), INDEXNO( "a" << 1 ), FBS( BSONObj() ), FBS2( BSONObj() ), BSONObj(), BSON( "a" << 1 ) );
                ASSERT( !p.exactKeyMatch() );
                QueryPlan p2( nsd(), INDEXNO( "b" << 1 << "a" << 1 ), FBS( BSONObj() ), FBS2( BSONObj() ), BSONObj(), BSON( "a" << 1 ) );
                ASSERT( !p2.exactKeyMatch() );
                QueryPlan p3( nsd(), INDEXNO( "b" << 1 << "a" << 1 ), FBS( BSON( "b" << "z" ) ), FBS2( BSON( "b" << "z" ) ), BSON( "b" << "z" ), BSON( "a" << 1 ) );
                ASSERT( !p3.exactKeyMatch() );
                QueryPlan p4( nsd(), INDEXNO( "b" << 1 << "a" << 1 << "c" << 1 ), FBS( BSON( "c" << "y" << "b" << "z" ) ), FBS2( BSON( "c" << "y" << "b" << "z" ) ), BSON( "c" << "y" << "b" << "z" ), BSON( "a" << 1 ) );
                ASSERT( !p4.exactKeyMatch() );
                QueryPlan p5( nsd(), INDEXNO( "b" << 1 << "a" << 1 << "c" << 1 ), FBS( BSON( "c" << "y" << "b" << "z" ) ), FBS2( BSON( "c" << "y" << "b" << "z" ) ), BSON( "c" << "y" << "b" << "z" ), BSONObj() );
                ASSERT( !p5.exactKeyMatch() );
                QueryPlan p6( nsd(), INDEXNO( "b" << 1 << "a" << 1 << "c" << 1 ), FBS( BSON( "c" << LT << "y" << "b" << GT << "z" ) ), FBS2( BSON( "c" << LT << "y" << "b" << GT << "z" ) ), BSON( "c" << LT << "y" << "b" << GT << "z" ), BSONObj() );
                ASSERT( !p6.exactKeyMatch() );
                QueryPlan p7( nsd(), INDEXNO( "b" << 1 ), FBS( BSONObj() ), FBS2( BSONObj() ), BSONObj(), BSON( "a" << 1 ) );
                ASSERT( !p7.exactKeyMatch() );
                QueryPlan p8( nsd(), INDEXNO( "a" << 1 << "b" << 1 ), FBS( BSON( "b" << "y" << "a" << "z" ) ), FBS2( BSON( "b" << "y" << "a" << "z" ) ), BSON( "b" << "y" << "a" << "z" ), BSONObj() );
                ASSERT( p8.exactKeyMatch() );
                QueryPlan p9( nsd(), INDEXNO( "a" << 1 ), FBS( BSON( "a" << "z" ) ), FBS2( BSON( "a" << "z" ) ), BSON( "a" << "z" ), BSON( "a" << 1 ) );
                ASSERT( p9.exactKeyMatch() );
            }
        };
        
        class MoreKeyMatch : public Base {
        public:
            void run() {
                QueryPlan p( nsd(), INDEXNO( "a" << 1 ), FBS( BSON( "a" << "r" << "b" << NE << "q" ) ), FBS2( BSON( "a" << "r" << "b" << NE << "q" ) ), BSON( "a" << "r" << "b" << NE << "q" ), BSON( "a" << 1 ) );
                ASSERT( !p.exactKeyMatch() );                
            }
        };
        
        class ExactKeyQueryTypes : public Base {
        public:
            void run() {
                QueryPlan p( nsd(), INDEXNO( "a" << 1 ), FBS( BSON( "a" << "b" ) ), FBS2( BSON( "a" << "b" ) ), BSON( "a" << "b" ), BSONObj() );
                ASSERT( p.exactKeyMatch() );
                QueryPlan p2( nsd(), INDEXNO( "a" << 1 ), FBS( BSON( "a" << 4 ) ), FBS2( BSON( "a" << 4 ) ), BSON( "a" << 4 ), BSONObj() );
                ASSERT( !p2.exactKeyMatch() );
                QueryPlan p3( nsd(), INDEXNO( "a" << 1 ), FBS( BSON( "a" << BSON( "c" << "d" ) ) ), FBS2( BSON( "a" << BSON( "c" << "d" ) ) ), BSON( "a" << BSON( "c" << "d" ) ), BSONObj() );
                ASSERT( !p3.exactKeyMatch() );
                BSONObjBuilder b;
                b.appendRegex( "a", "^ddd" );
                BSONObj q = b.obj();
                QueryPlan p4( nsd(), INDEXNO( "a" << 1 ), FBS( q ), FBS2( q ), q, BSONObj() );
                ASSERT( !p4.exactKeyMatch() );
                QueryPlan p5( nsd(), INDEXNO( "a" << 1 << "b" << 1 ), FBS( BSON( "a" << "z" << "b" << 4 ) ), FBS2( BSON( "a" << "z" << "b" << 4 ) ), BSON( "a" << "z" << "b" << 4 ), BSONObj() );
                ASSERT( !p5.exactKeyMatch() );
            }
        };
        
        class Unhelpful : public Base {
        public:
            void run() {
                QueryPlan p( nsd(), INDEXNO( "a" << 1 << "b" << 1 ), FBS( BSON( "b" << 1 ) ), FBS2( BSON( "b" << 1 ) ), BSON( "b" << 1 ), BSONObj() );
                ASSERT( !p.range( "a" ).nontrivial() );
                ASSERT( p.unhelpful() );
                QueryPlan p2( nsd(), INDEXNO( "a" << 1 << "b" << 1 ), FBS( BSON( "b" << 1 << "c" << 1 ) ), FBS2( BSON( "b" << 1 << "c" << 1 ) ), BSON( "b" << 1 << "c" << 1 ), BSON( "a" << 1 ) );
                ASSERT( !p2.scanAndOrderRequired() );
                ASSERT( !p2.range( "a" ).nontrivial() );
                ASSERT( !p2.unhelpful() );
                QueryPlan p3( nsd(), INDEXNO( "b" << 1 ), FBS( BSON( "b" << 1 << "c" << 1 ) ), FBS2( BSON( "b" << 1 << "c" << 1 ) ), BSON( "b" << 1 << "c" << 1 ), BSONObj() );
                ASSERT( p3.range( "b" ).nontrivial() );
                ASSERT( !p3.unhelpful() );
                QueryPlan p4( nsd(), INDEXNO( "b" << 1 << "c" << 1 ), FBS( BSON( "c" << 1 << "d" << 1 ) ), FBS2( BSON( "c" << 1 << "d" << 1 ) ), BSON( "c" << 1 << "d" << 1 ), BSONObj() );
                ASSERT( !p4.range( "b" ).nontrivial() );
                ASSERT( p4.unhelpful() );
            }
        };
        
    } // namespace QueryPlanTests

    namespace QueryPlanSetTests {
        class Base {
        public:
            Base() : _context( ns() ){
                string err;
                userCreateNS( ns(), BSONObj(), err, false );
            }
            virtual ~Base() {
                if ( !nsd() )
                    return;
                NamespaceDetailsTransient::_get( ns() ).clearQueryCache();
                string s( ns() );
                dropNS( s );
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
                auto_ptr< FieldRangeSet > frs( new FieldRangeSet( ns(), BSON( "a" << 4 ) ) );
                auto_ptr< FieldRangeSet > frsOrig( new FieldRangeSet( *frs ) );
                QueryPlanSet s( ns(), frs, frsOrig, BSON( "a" << 4 ), BSON( "b" << 1 ) );
                ASSERT_EQUALS( 1, s.nPlans() );
            }
        };
        
        class Optimal : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), false, "a_1" );
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), false, "b_2" );
                auto_ptr< FieldRangeSet > frs( new FieldRangeSet( ns(), BSON( "a" << 4 ) ) );
                auto_ptr< FieldRangeSet > frsOrig( new FieldRangeSet( *frs ) );
                QueryPlanSet s( ns(), frs, frsOrig, BSON( "a" << 4 ), BSONObj() );
                ASSERT_EQUALS( 1, s.nPlans() );                
            }
        };

        class NoOptimal : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), false, "a_1" );
                Helpers::ensureIndex( ns(), BSON( "b" << 1 ), false, "b_1" );
                auto_ptr< FieldRangeSet > frs( new FieldRangeSet( ns(), BSON( "a" << 4 ) ) );
                auto_ptr< FieldRangeSet > frsOrig( new FieldRangeSet( *frs ) );
                QueryPlanSet s( ns(), frs, frsOrig, BSON( "a" << 4 ), BSON( "b" << 1 ) );
                ASSERT_EQUALS( 3, s.nPlans() );
            }
        };

        class NoSpec : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), false, "a_1" );
                Helpers::ensureIndex( ns(), BSON( "b" << 1 ), false, "b_1" );
                auto_ptr< FieldRangeSet > frs( new FieldRangeSet( ns(), BSONObj() ) );
                auto_ptr< FieldRangeSet > frsOrig( new FieldRangeSet( *frs ) );
                QueryPlanSet s( ns(), frs, frsOrig, BSONObj(), BSONObj() );
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
                auto_ptr< FieldRangeSet > frs( new FieldRangeSet( ns(), BSON( "a" << 1 ) ) );
                auto_ptr< FieldRangeSet > frsOrig( new FieldRangeSet( *frs ) );
                QueryPlanSet s( ns(), frs, frsOrig, BSON( "a" << 1 ), BSON( "b" << 1 ), &e );
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
                auto_ptr< FieldRangeSet > frs( new FieldRangeSet( ns(), BSON( "a" << 1 ) ) );
                auto_ptr< FieldRangeSet > frsOrig( new FieldRangeSet( *frs ) );
                QueryPlanSet s( ns(), frs, frsOrig, BSON( "a" << 1 ), BSON( "b" << 1 ), &e );
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
                auto_ptr< FieldRangeSet > frs( new FieldRangeSet( ns(), BSON( "a" << 1 ) ) );
                auto_ptr< FieldRangeSet > frsOrig( new FieldRangeSet( *frs ) );
                QueryPlanSet s( ns(), frs, frsOrig, BSON( "a" << 1 ), BSON( "b" << 1 ), &e );
                ASSERT_EQUALS( 1, s.nPlans() );                
            }
        };

        class NaturalSort : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), false, "a_1" );
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), false, "b_2" );
                auto_ptr< FieldRangeSet > frs( new FieldRangeSet( ns(), BSON( "a" << 1 ) ) );
                auto_ptr< FieldRangeSet > frsOrig( new FieldRangeSet( *frs ) );
                QueryPlanSet s( ns(), frs, frsOrig, BSON( "a" << 1 ), BSON( "$natural" << 1 ) );
                ASSERT_EQUALS( 1, s.nPlans() );
            }
        };

        class BadHint : public Base {
        public:
            void run() {
                BSONObj b = BSON( "hint" << "a_1" );
                BSONElement e = b.firstElement();
                auto_ptr< FieldRangeSet > frs( new FieldRangeSet( ns(), BSON( "a" << 1 ) ) );
                auto_ptr< FieldRangeSet > frsOrig( new FieldRangeSet( *frs ) );
                ASSERT_EXCEPTION( QueryPlanSet s( ns(), frs, frsOrig, BSON( "a" << 1 ), BSON( "b" << 1 ), &e ),
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
                auto_ptr< FieldRangeSet > frs( new FieldRangeSet( ns(), BSON( "a" << 1 << "c" << 2 ) ) );
                auto_ptr< FieldRangeSet > frsOrig( new FieldRangeSet( *frs ) );
                QueryPlanSet s( ns(), frs, frsOrig, BSON( "a" << 1 << "c" << 2 ), BSONObj() );
                ASSERT_EQUALS( 2, s.nPlans() );                
            }
        };        
        
        class SingleException : public Base {
        public:
            void run() {
                Helpers::ensureIndex( ns(), BSON( "a" << 1 ), false, "a_1" );
                Helpers::ensureIndex( ns(), BSON( "b" << 1 ), false, "b_1" );
                auto_ptr< FieldRangeSet > frs( new FieldRangeSet( ns(), BSON( "a" << 4 ) ) );
                auto_ptr< FieldRangeSet > frsOrig( new FieldRangeSet( *frs ) );
                QueryPlanSet s( ns(), frs, frsOrig, BSON( "a" << 4 ), BSON( "b" << 1 ) );
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
                auto_ptr< FieldRangeSet > frs( new FieldRangeSet( ns(), BSON( "a" << 4 ) ) );
                auto_ptr< FieldRangeSet > frsOrig( new FieldRangeSet( *frs ) );
                QueryPlanSet s( ns(), frs, frsOrig, BSON( "a" << 4 ), BSON( "b" << 1 ) );
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
                
                auto_ptr< FieldRangeSet > frs( new FieldRangeSet( ns(), BSON( "a" << 4 ) ) );
                auto_ptr< FieldRangeSet > frsOrig( new FieldRangeSet( *frs ) );
                QueryPlanSet s( ns(), frs, frsOrig, BSON( "a" << 4 ), BSON( "b" << 1 ) );
                NoRecordTestOp original;
                s.runOp( original );
                nPlans( 3 );

                BSONObj hint = fromjson( "{hint:{$natural:1}}" );
                BSONElement hintElt = hint.firstElement();
                auto_ptr< FieldRangeSet > frs2( new FieldRangeSet( ns(), BSON( "a" << 4 ) ) );
                auto_ptr< FieldRangeSet > frsOrig2( new FieldRangeSet( *frs2 ) );
                QueryPlanSet s2( ns(), frs2, frsOrig2, BSON( "a" << 4 ), BSON( "b" << 1 ), &hintElt );
                TestOp newOriginal;
                s2.runOp( newOriginal );
                nPlans( 3 );

                auto_ptr< FieldRangeSet > frs3( new FieldRangeSet( ns(), BSON( "a" << 4 ) ) );
                auto_ptr< FieldRangeSet > frsOrig3( new FieldRangeSet( *frs3 ) );
                QueryPlanSet s3( ns(), frs3, frsOrig3, BSON( "a" << 4 ), BSON( "b" << 1 << "c" << 1 ) );
                TestOp newerOriginal;
                s3.runOp( newerOriginal );
                nPlans( 3 );                
                
                runQuery();
                nPlans( 1 );
            }
        private:
            void nPlans( int n ) {
                auto_ptr< FieldRangeSet > frs( new FieldRangeSet( ns(), BSON( "a" << 4 ) ) );
                auto_ptr< FieldRangeSet > frsOrig( new FieldRangeSet( *frs ) );
                QueryPlanSet s( ns(), frs, frsOrig, BSON( "a" << 4 ), BSON( "b" << 1 ) );
                ASSERT_EQUALS( n, s.nPlans() );                
            }
            void runQuery() {
                auto_ptr< FieldRangeSet > frs( new FieldRangeSet( ns(), BSON( "a" << 4 ) ) );
                auto_ptr< FieldRangeSet > frsOrig( new FieldRangeSet( *frs ) );
                QueryPlanSet s( ns(), frs, frsOrig, BSON( "a" << 4 ), BSON( "b" << 1 ) );
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

                auto_ptr< FieldRangeSet > frs( new FieldRangeSet( ns(), BSON( "a" << 4 ) ) );
                auto_ptr< FieldRangeSet > frsOrig( new FieldRangeSet( *frs ) );
                QueryPlanSet s( ns(), frs, frsOrig, BSON( "a" << 4 ), BSON( "b" << 1 ) );
                ScanOnlyTestOp op;
                s.runOp( op );
                ASSERT( fromjson( "{$natural:1}" ).woCompare( NamespaceDetailsTransient::_get( ns() ).indexForPattern( s.fbs().pattern( BSON( "b" << 1 ) ) ) ) == 0 );
                ASSERT_EQUALS( 1, NamespaceDetailsTransient::_get( ns() ).nScannedForPattern( s.fbs().pattern( BSON( "b" << 1 ) ) ) );
                
                auto_ptr< FieldRangeSet > frs2( new FieldRangeSet( ns(), BSON( "a" << 4 ) ) );
                auto_ptr< FieldRangeSet > frsOrig2( new FieldRangeSet( *frs2 ) );
                QueryPlanSet s2( ns(), frs2, frsOrig2, BSON( "a" << 4 ), BSON( "b" << 1 ) );
                TestOp op2;
                ASSERT( s2.runOp( op2 )->complete() );
            }
        private:
            class TestOp : public QueryOp {
            public:
                TestOp() {}
                virtual void _init() {}
                virtual void next() {
                    if ( qp().indexKey().firstElement().fieldName() == string( "$natural" ) )
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
                ASSERT( BSON( "a" << 1 ).woCompare( NamespaceDetailsTransient::_get( ns() ).indexForPattern( FieldRangeSet( ns(), BSON( "a" << 1 ) ).pattern() ) ) == 0 );
                ASSERT_EQUALS( 1, NamespaceDetailsTransient::_get( ns() ).nScannedForPattern( FieldRangeSet( ns(), BSON( "a" << 1 ) ).pattern() ) );
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
                ASSERT( BSON( "$natural" << 1 ).woCompare( NamespaceDetailsTransient::_get( ns() ).indexForPattern( FieldRangeSet( ns(), BSON( "b" << 0 << "a" << GTE << 0 ) ).pattern() ) ) == 0 );
                
                Message m2;
                assembleRequest( ns(), QUERY( "b" << 99 << "a" << GTE << 0 ).obj, 2, 0, 0, 0, m2 );
                {
                    DbMessage d(m2);
                    QueryMessage q(d);
                    runQuery( m2, q);
                }
                ASSERT( BSON( "a" << 1 ).woCompare( NamespaceDetailsTransient::_get( ns() ).indexForPattern( FieldRangeSet( ns(), BSON( "b" << 0 << "a" << GTE << 0 ) ).pattern() ) ) == 0 );                
                ASSERT_EQUALS( 3, NamespaceDetailsTransient::_get( ns() ).nScannedForPattern( FieldRangeSet( ns(), BSON( "b" << 0 << "a" << GTE << 0 ) ).pattern() ) );
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
                auto_ptr< FieldRangeSet > frs( new FieldRangeSet( ns(), fromjson( "{a:{$in:[2,3,6,9,11]}}" ) ) );
                auto_ptr< FieldRangeSet > frsOrig( new FieldRangeSet( *frs ) );
                QueryPlanSet s( ns(), frs, frsOrig, fromjson( "{a:{$in:[2,3,6,9,11]}}" ), BSONObj(), &hintElt );
                QueryPlan qp( nsd(), 1, s.fbs(), s.originalFrs(), fromjson( "{a:{$in:[2,3,6,9,11]}}" ), BSONObj() );
                boost::shared_ptr<Cursor> c = qp.newCursor();
                double expected[] = { 2, 3, 6, 9 };
                for( int i = 0; i < 4; ++i, c->advance() ) {
                    ASSERT_EQUALS( expected[ i ], c->current().getField( "a" ).number() );
                }
                ASSERT( !c->ok() );
                
                // now check reverse
                {
                    auto_ptr< FieldRangeSet > frs( new FieldRangeSet( ns(), fromjson( "{a:{$in:[2,3,6,9,11]}}" ) ) );
                    auto_ptr< FieldRangeSet > frsOrig( new FieldRangeSet( *frs ) );
                    QueryPlanSet s( ns(), frs, frsOrig, fromjson( "{a:{$in:[2,3,6,9,11]}}" ), BSON( "a" << -1 ), &hintElt );
                    QueryPlan qp( nsd(), 1, s.fbs(), s.originalFrs(), fromjson( "{a:{$in:[2,3,6,9,11]}}" ), BSON( "a" << -1 ) );
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
                BSONElement hintElt = hint.firstElement();
                auto_ptr< FieldRangeSet > frs( new FieldRangeSet( ns(), fromjson( "{a:5,b:{$in:[2,3,6,9,11]}}" ) ) );                
                QueryPlan qp( nsd(), 1, *frs, *frs, fromjson( "{a:5,b:{$in:[2,3,6,9,11]}}" ), BSONObj() );
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
                BSONElement hintElt = hint.firstElement();
                auto_ptr< FieldRangeSet > frs( new FieldRangeSet( ns(), fromjson( "{a:{$gte:5},b:{$in:[2,3,6,9,11]}}" ) ) );
                QueryPlan qp( nsd(), 1, *frs, *frs, fromjson( "{a:{$gte:5},b:{$in:[2,3,6,9,11]}}" ), BSONObj() );
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
            dropNS( s );
        }
    protected:
        static const char *ns() { return "unittests.BaseTests"; }
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
            ASSERT_EQUALS( string( "b" ), c->indexKeyPattern().firstElement().fieldName() );
            boost::shared_ptr< MultiCursor > m = dynamic_pointer_cast< MultiCursor >( bestGuessCursor( ns(), fromjson( "{b:1,$or:[{z:1}]}" ), BSON( "a" << 1 ) ) );
            ASSERT_EQUALS( string( "a" ), m->sub_c()->indexKeyPattern().firstElement().fieldName() );
            m = dynamic_pointer_cast< MultiCursor >( bestGuessCursor( ns(), fromjson( "{a:1,$or:[{y:1}]}" ), BSON( "b" << 1 ) ) );
            ASSERT_EQUALS( string( "b" ), m->sub_c()->indexKeyPattern().firstElement().fieldName() );
            
            FieldRangeSet frs( "ns", BSON( "a" << 1 ) );
            {
                scoped_lock lk(NamespaceDetailsTransient::_qcMutex);
                NamespaceDetailsTransient::get_inlock( ns() ).registerIndexForPattern( frs.pattern( BSON( "b" << 1 ) ), BSON( "a" << 1 ), 0 );  
            }
            m = dynamic_pointer_cast< MultiCursor >( bestGuessCursor( ns(), fromjson( "{a:1,$or:[{y:1}]}" ), BSON( "b" << 1 ) ) );
            ASSERT_EQUALS( string( "b" ), m->sub_c()->indexKeyPattern().firstElement().fieldName() );
        }
    };
    
    class All : public Suite {
    public:
        All() : Suite( "queryoptimizer" ){}
        
        void setupTests(){
            add< FieldRangeTests::Empty >();
            add< FieldRangeTests::Eq >();
            add< FieldRangeTests::DupEq >();
            add< FieldRangeTests::Lt >();
            add< FieldRangeTests::Lte >();
            add< FieldRangeTests::Gt >();
            add< FieldRangeTests::Gte >();
            add< FieldRangeTests::TwoLt >();
            add< FieldRangeTests::TwoGt >();
            add< FieldRangeTests::EqGte >();
            add< FieldRangeTests::EqGteInvalid >();
            add< FieldRangeTests::Regex >();
            add< FieldRangeTests::RegexObj >();
            add< FieldRangeTests::UnhelpfulRegex >();
            add< FieldRangeTests::In >();
            add< FieldRangeTests::Equality >();
            add< FieldRangeTests::SimplifiedQuery >();
            add< FieldRangeTests::QueryPatternTest >();
            add< FieldRangeTests::NoWhere >();
            add< FieldRangeTests::Numeric >();
            add< FieldRangeTests::InLowerBound >();
            add< FieldRangeTests::InUpperBound >();
            add< FieldRangeTests::UnionBound >();
            add< FieldRangeTests::MultiBound >();
            add< FieldRangeTests::Diff1 >();
            add< FieldRangeTests::Diff2 >();
            add< FieldRangeTests::Diff3 >();
            add< FieldRangeTests::Diff4 >();
            add< FieldRangeTests::Diff5 >();
            add< FieldRangeTests::Diff6 >();
            add< FieldRangeTests::Diff7 >();
            add< FieldRangeTests::Diff8 >();
            add< FieldRangeTests::Diff9 >();
            add< FieldRangeTests::Diff10 >();
            add< FieldRangeTests::Diff11 >();
            add< FieldRangeTests::Diff12 >();
            add< FieldRangeTests::Diff13 >();
            add< FieldRangeTests::Diff14 >();
            add< FieldRangeTests::Diff15 >();
            add< FieldRangeTests::Diff16 >();
            add< FieldRangeTests::Diff17 >();
            add< FieldRangeTests::Diff18 >();
            add< FieldRangeTests::Diff19 >();
            add< FieldRangeTests::Diff20 >();
            add< FieldRangeTests::Diff21 >();
            add< FieldRangeTests::Diff22 >();
            add< FieldRangeTests::Diff23 >();
            add< FieldRangeTests::Diff24 >();
            add< FieldRangeTests::Diff25 >();
            add< FieldRangeTests::Diff26 >();
            add< FieldRangeTests::Diff27 >();
            add< FieldRangeTests::Diff28 >();
            add< FieldRangeTests::Diff29 >();
            add< FieldRangeTests::Diff30 >();
            add< FieldRangeTests::Diff31 >();
            add< FieldRangeTests::Diff32 >();
            add< FieldRangeTests::Diff33 >();
            add< FieldRangeTests::Diff34 >();
            add< FieldRangeTests::Diff35 >();
            add< FieldRangeTests::Diff36 >();
            add< FieldRangeTests::Diff37 >();
            add< FieldRangeTests::Diff38 >();
            add< FieldRangeTests::Diff39 >();
            add< FieldRangeTests::Diff40 >();
            add< FieldRangeTests::Diff41 >();
            add< FieldRangeTests::Diff42 >();
            add< FieldRangeTests::Diff43 >();
            add< FieldRangeTests::Diff44 >();
            add< FieldRangeTests::Diff45 >();
            add< FieldRangeTests::Diff46 >();
            add< FieldRangeTests::Diff47 >();
            add< FieldRangeTests::Diff48 >();
            add< FieldRangeTests::Diff49 >();
            add< FieldRangeTests::Diff50 >();
            add< FieldRangeTests::Diff51 >();
            add< FieldRangeTests::Diff52 >();
            add< FieldRangeTests::Diff53 >();
            add< FieldRangeTests::Diff54 >();
            add< FieldRangeTests::Diff55 >();
            add< FieldRangeTests::Diff56 >();
            add< FieldRangeTests::Diff57 >();
            add< FieldRangeTests::Diff58 >();
            add< FieldRangeTests::Diff59 >();
            add< FieldRangeTests::Diff60 >();
            add< FieldRangeTests::Diff61 >();
            add< FieldRangeTests::Diff62 >();
            add< FieldRangeTests::Diff63 >();
            add< FieldRangeTests::Diff64 >();
            add< FieldRangeTests::DiffMulti1 >();
            add< FieldRangeTests::DiffMulti2 >();
            add< FieldRangeTests::SetIntersect >();
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
            add< QueryPlanTests::MoreKeyMatch >();
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
            add< QueryPlanSetTests::TryAllPlansOnErr >();
            add< QueryPlanSetTests::FindOne >();
            add< QueryPlanSetTests::Delete >();
            add< QueryPlanSetTests::DeleteOneScan >();
            add< QueryPlanSetTests::DeleteOneIndex >();
            add< QueryPlanSetTests::TryOtherPlansBeforeFinish >();
            add< QueryPlanSetTests::InQueryIntervals >();
            add< QueryPlanSetTests::EqualityThenIn >();
            add< QueryPlanSetTests::NotEqualityThenIn >();
            add< BestGuess >();
        }
    } myall;
    
} // namespace QueryOptimizerTests

