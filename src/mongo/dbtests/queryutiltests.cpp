// queryutiltests.cpp : query utility unit tests
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
#include "../db/queryutil.h"
#include "../db/querypattern.h"
#include "../db/instance.h"
#include "../db/pdfile.h"
#include "dbtests.h"

namespace QueryUtilTests {

    namespace FieldRangeTests {
        class Base {
        public:
            virtual ~Base() {}
            void run() {
                const FieldRangeSet s( "ns", query(), true );
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
            NumericBase() {
                o = BSON( "min" << -numeric_limits<double>::max() << "max" << numeric_limits<double>::max() );
            }

            virtual BSONElement lower() { return o["min"]; }
            virtual BSONElement upper() { return o["max"]; }
        private:
            BSONObj o;
        };

        class EmptyQuery : public Base {
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
                FieldRangeSet frs( "ns", BSON( "a" << 1 << "a" << GTE << 2 ), true );
                ASSERT( !frs.matchPossible() );
            }
        };

        struct RegexBase : Base {
            void run() { //need to only look at first interval
                FieldRangeSet s( "ns", query(), true );
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

        class Empty {
        public:
            void run() {
                FieldRangeSet s( "ns", BSON( "a" << GT << 5 << LT << 5 ), true );
                const FieldRange &r = s.range( "a" );
                ASSERT( r.empty() );
                ASSERT( !r.nontrivial() ); // SERVER-4556
            }
        };
        
        class Equality {
        public:
            void run() {
                FieldRangeSet s( "ns", BSON( "a" << 1 ), true );
                ASSERT( s.range( "a" ).equality() );
                FieldRangeSet s2( "ns", BSON( "a" << GTE << 1 << LTE << 1 ), true );
                ASSERT( s2.range( "a" ).equality() );
                FieldRangeSet s3( "ns", BSON( "a" << GT << 1 << LTE << 1 ), true );
                ASSERT( !s3.range( "a" ).equality() );
                FieldRangeSet s4( "ns", BSON( "a" << GTE << 1 << LT << 1 ), true );
                ASSERT( !s4.range( "a" ).equality() );
                FieldRangeSet s5( "ns", BSON( "a" << GTE << 1 << LTE << 1 << GT << 1 ), true );
                ASSERT( !s5.range( "a" ).equality() );
                FieldRangeSet s6( "ns", BSON( "a" << GTE << 1 << LTE << 1 << LT << 1 ), true );
                ASSERT( !s6.range( "a" ).equality() );
            }
        };

        class SimplifiedQuery {
        public:
            void run() {
                FieldRangeSet frs( "ns", BSON( "a" << GT << 1 << GT << 5 << LT << 10 << "b" << 4 << "c" << LT << 4 << LT << 6 << "d" << GTE << 0 << GT << 0 << "e" << GTE << 0 << LTE << 10 ), true );
                BSONObj simple = frs.simplifiedQuery();
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
                return FieldRangeSet( "", query, true ).pattern( sort );
            }
        };

        class NoWhere {
        public:
            void run() {
                ASSERT_EQUALS( 0, FieldRangeSet( "ns", BSON( "$where" << 1 ), true ).nNontrivialRanges() );
            }
        };

        class Numeric {
        public:
            void run() {
                FieldRangeSet f( "", BSON( "a" << 1 ), true );
                ASSERT( f.range( "a" ).min().woCompare( BSON( "a" << 2.0 ).firstElement() ) < 0 );
                ASSERT( f.range( "a" ).min().woCompare( BSON( "a" << 0.0 ).firstElement() ) > 0 );
            }
        };

        class InLowerBound {
        public:
            void run() {
                FieldRangeSet f( "", fromjson( "{a:{$gt:4,$in:[1,2,3,4,5,6]}}" ), true );
                ASSERT( f.range( "a" ).min().woCompare( BSON( "a" << 5.0 ).firstElement(), false ) == 0 );
                ASSERT( f.range( "a" ).max().woCompare( BSON( "a" << 6.0 ).firstElement(), false ) == 0 );
            }
        };

        class InUpperBound {
        public:
            void run() {
                FieldRangeSet f( "", fromjson( "{a:{$lt:4,$in:[1,2,3,4,5,6]}}" ), true );
                ASSERT( f.range( "a" ).min().woCompare( BSON( "a" << 1.0 ).firstElement(), false ) == 0 );
                ASSERT( f.range( "a" ).max().woCompare( BSON( "a" << 3.0 ).firstElement(), false ) == 0 );
            }
        };

        class UnionBound {
        public:
            void run() {
                FieldRangeSet frs( "", fromjson( "{a:{$gt:1,$lt:9},b:{$gt:9,$lt:12}}" ), true );
                FieldRange ret = frs.range( "a" );
                ret |= frs.range( "b" );
                ASSERT_EQUALS( 2U, ret.intervals().size() );
            }
        };

        class MultiBound {
        public:
            void run() {
                FieldRangeSet frs1( "", fromjson( "{a:{$in:[1,3,5,7,9]}}" ), true );
                FieldRangeSet frs2( "", fromjson( "{a:{$in:[2,3,5,8,9]}}" ), true );
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
                FieldRangeSet frs( "", fromjson( obj().toString() ), true );
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
                FieldRangeSet frs( "", fromjson( "{a:{$gt:1,$lt:9},b:{$gt:0,$lt:2},c:3,d:{$gt:4,$lt:5},e:{$gt:7,$lt:10}}" ), true );
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
                FieldRangeSet frs( "", fromjson( "{a:{$gt:1,$lt:9},b:{$gt:0,$lt:2},c:3,d:{$gt:4,$lt:5},e:{$gt:7,$lt:10}}" ), true );
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

    } // namespace FieldRangeTests

    namespace FieldRangeSetTests {

        class Intersect {
        public:
            void run() {
                FieldRangeSet frs1( "", fromjson( "{b:{$in:[5,6]},c:7,d:{$in:[8,9]}}" ), true );
                FieldRangeSet frs2( "", fromjson( "{a:1,b:5,c:{$in:[7,8]},d:{$in:[8,9]},e:10}" ), true );
                frs1 &= frs2;
                ASSERT_EQUALS( fromjson( "{a:1,b:5,c:7,d:{$gte:8,$lte:9},e:10}" ), frs1.simplifiedQuery( BSONObj() ) );
            }
        };
        
        class MultiKeyIntersect {
        public:
            void run() {
                FieldRangeSet frs1( "", BSONObj(), false );
                FieldRangeSet frs2( "", BSON( "a" << GT << 4 ), false );
                FieldRangeSet frs3( "", BSON( "a" << LT << 6 ), false );
                // An intersection with a trivial range is allowed.
                frs1 &= frs2;
                ASSERT_EQUALS( frs2.simplifiedQuery( BSONObj() ), frs1.simplifiedQuery( BSONObj() ) );
                // An intersection with a nontrivial range is not allowed, as it might prevent a valid
                // multikey match.
                frs1 &= frs3;
                ASSERT_EQUALS( frs2.simplifiedQuery( BSONObj() ), frs1.simplifiedQuery( BSONObj() ) );
                // Now intersect with a fully contained range.
                FieldRangeSet frs4( "", BSON( "a" << GT << 6 ), false );
                frs1 &= frs4;
                ASSERT_EQUALS( frs4.simplifiedQuery( BSONObj() ), frs1.simplifiedQuery( BSONObj() ) );                
            }
        };
        
        class MultiKeyDiff {
        public:
            void run() {
                FieldRangeSet frs1( "", BSON( "a" << GT << 4 ), false );
                FieldRangeSet frs2( "", BSON( "a" << GT << 6 ), false );
                // Range subtraction is no different for multikey ranges.
                frs1 -= frs2;
                ASSERT_EQUALS( BSON( "a" << GT << 4 << LTE << 6 ), frs1.simplifiedQuery( BSONObj() ) );
            }
        };
        
        class MatchPossible {
        public:
            void run() {
                FieldRangeSet frs1( "", BSON( "a" << GT << 4 ), true );
                ASSERT( frs1.matchPossible() );
                // Conflicting constraints invalid for a single key set.
                FieldRangeSet frs2( "", BSON( "a" << GT << 4 << LT << 2 ), true );
                ASSERT( !frs2.matchPossible() );
                // Conflicting constraints not possible for a multi key set.
                FieldRangeSet frs3( "", BSON( "a" << GT << 4 << LT << 2 ), false );
                ASSERT( frs3.matchPossible() );
            }
        };
        
        class MatchPossibleForIndex {
        public:
            void run() {
                // Conflicting constraints not possible for a multi key set.
                FieldRangeSet frs1( "", BSON( "a" << GT << 4 << LT << 2 ), false );
                ASSERT( frs1.matchPossibleForIndex( BSON( "a" << 1 ) ) );
                // Conflicting constraints for a multi key set.
                FieldRangeSet frs2( "", BSON( "a" << GT << 4 << LT << 2 ), true );
                ASSERT( !frs2.matchPossibleForIndex( BSON( "a" << 1 ) ) );
                // If the index doesn't include the key, it is not single key invalid.
                ASSERT( frs2.matchPossibleForIndex( BSON( "b" << 1 ) ) );
                // If the index key is not an index, the set is not single key invalid.
                ASSERT( frs2.matchPossibleForIndex( BSON( "$natural" << 1 ) ) );
                ASSERT( frs2.matchPossibleForIndex( BSONObj() ) );
            }
        };
                
    } // namespace FieldRangeSetTests
    
    namespace FieldRangeSetPairTests {

        class NoNontrivialRanges {
        public:
            void run() {
                FieldRangeSetPair frsp1( "", BSONObj() );
                ASSERT( frsp1.noNontrivialRanges() );
                FieldRangeSetPair frsp2( "", BSON( "a" << 1 ) );
                ASSERT( !frsp2.noNontrivialRanges() );
                FieldRangeSetPair frsp3( "", BSON( "a" << GT << 1 ) );
                ASSERT( !frsp3.noNontrivialRanges() );
                // A single key invalid constraint is still nontrivial.
                FieldRangeSetPair frsp4( "", BSON( "a" << GT << 1 << LT << 0 ) );
                ASSERT( !frsp4.noNontrivialRanges() );
                // Still nontrivial if multikey invalid.
                frsp4 -= frsp4.frsForIndex( 0, -1 );
                ASSERT( !frsp4.noNontrivialRanges() );
            }
        };
        
        class MatchPossible {
        public:
            void run() {
                // Match possible for simple query.
                FieldRangeSetPair frsp1( "", BSON( "a" << 1 ) );
                ASSERT( frsp1.matchPossible() );
                // Match possible for single key invalid query.
                FieldRangeSetPair frsp2( "", BSON( "a" << GT << 1 << LT << 0 ) );
                ASSERT( frsp2.matchPossible() );
                // Match not possible for multi key invalid query.
                frsp1 -= frsp1.frsForIndex( 0, - 1 );
                ASSERT( !frsp1.matchPossible() );
            }
        };

        class IndexBase {
        public:
            IndexBase() : _ctx( ns() ) , indexNum_( 0 ) {
                string err;
                userCreateNS( ns(), BSONObj(), err, false );
            }
            ~IndexBase() {
                if ( !nsd() )
                    return;
                string s( ns() );
                dropNS( s );
            }
        protected:
            static const char *ns() { return "unittests.FieldRangeSetPairTests"; }
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
            static DBDirectClient client_;
        private:
            dblock lk_;
            Client::Context _ctx;
            int indexNum_;
        };
        DBDirectClient IndexBase::client_;
        
        class MatchPossibleForIndex : public IndexBase {
        public:
            void run() {
                int a = indexno( BSON( "a" << 1 ) );
                int b = indexno( BSON( "b" << 1 ) );
                IndexBase::client_.insert( ns(), BSON( "a" << BSON_ARRAY( 1 << 2 ) << "b" << 1 ) );
                // Valid ranges match possible for both indexes.
                FieldRangeSetPair frsp1( ns(), BSON( "a" << GT << 1 << LT << 4 << "b" << GT << 1 << LT << 4 ) );
                ASSERT( frsp1.matchPossibleForIndex( nsd(), a, BSON( "a" << 1 ) ) );
                ASSERT( frsp1.matchPossibleForIndex( nsd(), b, BSON( "b" << 1 ) ) );
                // Single key invalid range means match impossible for single key index.
                FieldRangeSetPair frsp2( ns(), BSON( "a" << GT << 4 << LT << 1 << "b" << GT << 4 << LT << 1 ) );
                ASSERT( frsp2.matchPossibleForIndex( nsd(), a, BSON( "a" << 1 ) ) );
                ASSERT( !frsp2.matchPossibleForIndex( nsd(), b, BSON( "b" << 1 ) ) );
            }
        };
        
    } // namespace FieldRangeSetPairTests
    
    class All : public Suite {
    public:
        All() : Suite( "queryutil" ) {}

        void setupTests() {
            add< FieldRangeTests::EmptyQuery >();
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
            add< FieldRangeTests::Empty >();
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
            add< FieldRangeSetTests::Intersect >();
            add< FieldRangeSetTests::MultiKeyIntersect >();
            add< FieldRangeSetTests::MultiKeyDiff >();
            add< FieldRangeSetTests::MatchPossible >();
            add< FieldRangeSetTests::MatchPossibleForIndex >();
            add< FieldRangeSetPairTests::NoNontrivialRanges >();
            add< FieldRangeSetPairTests::MatchPossible >();
            add< FieldRangeSetPairTests::MatchPossibleForIndex >();
        }
    } myall;

} // namespace QueryUtilTests

