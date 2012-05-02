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
#include "mongo/db/queryoptimizer.h"
#include "../db/querypattern.h"
#include "../db/instance.h"
#include "../db/pdfile.h"
#include "mongo/db/json.h"
#include "dbtests.h"

namespace QueryUtilTests {

    namespace FieldIntervalTests {
        class ToString {
        public:
            void run() {
                BSONObj obj = BSON( "a" << 1 );
                FieldInterval fieldInterval( obj.firstElement() );
                fieldInterval.toString(); // Just test that we don't crash.
            }
        };
    } // namespace FieldIntervalTests
    
    namespace FieldRangeTests {
        class ToString {
        public:
            void run() {
                BSONObj obj = BSON( "a" << 1 );
                FieldRange fieldRange( obj.firstElement(), true );
                fieldRange.toString(); // Just test that we don't crash.
            }
        };        
        
        class Base {
        public:
            virtual ~Base() {}
            void run() {
                const FieldRangeSet s( "ns", query(), true );
                checkElt( lower(), s.range( "a" ).min() );
                checkElt( upper(), s.range( "a" ).max() );
                ASSERT_EQUALS( lowerInclusive(), s.range( "a" ).minInclusive() );
                ASSERT_EQUALS( upperInclusive(), s.range( "a" ).maxInclusive() );
                ASSERT_EQUALS( simpleFiniteSet(), s.range( "a" ).simpleFiniteSet() );
            }
        protected:
            virtual BSONObj query() = 0;
            virtual BSONElement lower() { return minKey.firstElement(); }
            virtual bool lowerInclusive() { return true; }
            virtual BSONElement upper() { return maxKey.firstElement(); }
            virtual bool upperInclusive() { return true; }
            virtual bool simpleFiniteSet() { return false; }
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
            virtual bool simpleFiniteSet() { return true; }
            BSONObj o_;
        };

        class DupEq : public Eq {
        public:
            virtual BSONObj query() { return BSON( "a" << 1 << "b" << 2 << "a" << 1 ); }
            virtual bool simpleFiniteSet() { return false; }
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
            virtual bool simpleFiniteSet() { return false; }
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
            virtual bool simpleFiniteSet() { return true; }
            BSONObj o1_, o2_;
        };

        class And : public Base {
        public:
            And() : _o1( BSON( "-" << 0 ) ), _o2( BSON( "-" << 10 ) ) {}
            void run() {
                Base::run();
                const FieldRangeSet s( "ns", query(), true );
                // There should not be an index constraint recorded for the $and field.
                ASSERT( s.range( "$and" ).universal() );
            }
        private:
            virtual BSONObj query() {
                return BSON( "$and" <<
                            BSON_ARRAY( BSON( "a" << GT << 0 ) << BSON( "a" << LTE << 10 ) ) );
            }
            virtual BSONElement lower() { return _o1.firstElement(); }
            virtual bool lowerInclusive() { return false; }
            virtual BSONElement upper() { return _o2.firstElement(); }
            BSONObj _o1, _o2;
        };
        
        class Empty {
        public:
            void run() {
                FieldRangeSet s( "ns", BSON( "a" << GT << 5 << LT << 5 ), true );
                const FieldRange &r = s.range( "a" );
                ASSERT( r.empty() );
                ASSERT( !r.equality() );
                ASSERT( !r.universal() );
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
                FieldRangeSet frs( "ns",
                                  BSON( "a" << GT << 1 << GT << 5 << LT << 10 <<
                                       "b" << 4 <<
                                       "c" << LT << 4 << LT << 6 <<
                                       "d" << GTE << 0 << GT << 0 <<
                                       "e" << GTE << 0 << LTE << 10 <<
                                       "f" << NE << 9 ),
                                  true );
                BSONObj simple = frs.simplifiedQuery();
                ASSERT_EQUALS( fromjson( "{$gt:5,$lt:10}" ), simple.getObjectField( "a" ) );
                ASSERT_EQUALS( 4, simple.getIntField( "b" ) );
                ASSERT_EQUALS( BSON("$gte" << -numeric_limits<double>::max() << "$lt" << 4 ),
                              simple.getObjectField( "c" ) );
                ASSERT_EQUALS( BSON("$gt" << 0 << "$lte" << numeric_limits<double>::max() ),
                              simple.getObjectField( "d" ) );
                ASSERT_EQUALS( fromjson( "{$gte:0,$lte:10}" ),
                              simple.getObjectField( "e" ) );
                ASSERT_EQUALS( BSON( "$gte" << MINKEY << "$lte" << MAXKEY ),
                              simple.getObjectField( "f" ) );
            }
        };

        class QueryPatternBase {
        protected:
            static QueryPattern p( const BSONObj &query, const BSONObj &sort = BSONObj() ) {
                return FieldRangeSet( "", query, true ).pattern( sort );
            }
        };

        class QueryPatternTest : public QueryPatternBase {
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
        };

        class QueryPatternEmpty : public QueryPatternBase {
        public:
            void run() {
                ASSERT( p( BSON( "a" << GT << 5 << LT << 7 ) ) !=
                       p( BSON( "a" << GT << 7 << LT << 5 ) ) );
            }
        };

        class QueryPatternNeConstraint : public QueryPatternBase {
        public:
            void run() {
                ASSERT( p( BSON( "a" << NE << 5 ) ) != p( BSON( "a" << GT << 1 ) ) );
                ASSERT( p( BSON( "a" << NE << 5 ) ) != p( BSONObj() ) );
                ASSERT( p( BSON( "a" << NE << 5 ) ) == p( BSON( "a" << NE << "a" ) ) );
            }
        };
        
        /** Check QueryPattern categories for optimized bounds. */
        class QueryPatternOptimizedBounds {
        public:
            void run() {
                // With unoptimized bounds, different inequalities yield different query patterns.
                ASSERT( p( BSON( "a" << GT << 1 ), false ) != p( BSON( "a" << LT << 1 ), false ) );
                // SERVER-4675 Descriptive test - With optimized bounds, different inequalities
                // yield different query patterns.
                ASSERT( p( BSON( "a" << GT << 1 ), true ) == p( BSON( "a" << LT << 1 ), true ) );
            }
        private:
            static QueryPattern p( const BSONObj &query, bool optimize ) {
                return FieldRangeSet( "", query, true, optimize ).pattern( BSONObj() );
            }
        };

        class NoWhere {
        public:
            void run() {
                ASSERT_EQUALS( 0, FieldRangeSet( "ns", BSON( "$where" << 1 ), true ).numNonUniversalRanges() );
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

        /** Check union of two non overlapping ranges. */
        class BoundUnion {
        public:
            void run() {
                FieldRangeSet frs( "", fromjson( "{a:{$gt:1,$lt:9},b:{$gt:9,$lt:12}}" ), true );
                FieldRange ret = frs.range( "a" );
                ret |= frs.range( "b" );
                ASSERT_EQUALS( 2U, ret.intervals().size() );
            }
        };

        /** Check union of two ranges where one includes another. */
        class BoundUnionFullyContained {
        public:
            void run() {
                FieldRangeSet frs( "", fromjson( "{a:{$gt:1,$lte:9},b:{$gt:2,$lt:8},c:{$gt:2,$lt:9},d:{$gt:2,$lte:9}}" ), true );
                FieldRange u = frs.range( "a" );
                u |= frs.range( "b" );
                ASSERT_EQUALS( 1U, u.intervals().size() );
                ASSERT_EQUALS( frs.range( "a" ).toString(), u.toString() );
                u |= frs.range( "c" );
                ASSERT_EQUALS( 1U, u.intervals().size() );
                ASSERT_EQUALS( frs.range( "a" ).toString(), u.toString() );
                u |= frs.range( "d" );
                ASSERT_EQUALS( 1U, u.intervals().size() );
                ASSERT_EQUALS( frs.range( "a" ).toString(), u.toString() );
            }
        };

        /**
         * Check union of two ranges where one does not include another because of an inclusive
         * bound.
         */
        class BoundUnionOverlapWithInclusivity {
        public:
            void run() {
                FieldRangeSet frs( "", fromjson( "{a:{$gt:1,$lt:9},b:{$gt:2,$lte:9}}" ), true );
                FieldRange u = frs.range( "a" );
                u |= frs.range( "b" );
                ASSERT_EQUALS( 1U, u.intervals().size() );
                FieldRangeSet expected( "", fromjson( "{a:{$gt:1,$lte:9}}" ), true );
                ASSERT_EQUALS( expected.range( "a" ).toString(), u.toString() );
            }
        };
        
        /** Check union of two empty ranges. */
        class BoundUnionEmpty {
        public:
            void run() {
                FieldRangeSet frs( "", fromjson( "{a:{$in:[]},b:{$in:[]}}" ), true );
                FieldRange a = frs.range( "a" );
                a |= frs.range( "b" );
                ASSERT( a.empty() );
                ASSERT_EQUALS( 0U, a.intervals().size() );
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
        
        class Universal {
        public:
            void run() {
                FieldRangeSet frs1( "", BSON( "a" << 1 ), true );
                FieldRange f1 = frs1.range( "a" );
                ASSERT( !f1.universal() );
                ASSERT( frs1.range( "b" ).universal() );
                
                FieldRangeSet frs2( "", BSON( "a" << GT << 1 ), true );
                FieldRange f2 = frs2.range( "a" );
                ASSERT( !f2.universal() );

                FieldRangeSet frs3( "", BSON( "a" << LT << 1 ), true );
                FieldRange f3 = frs3.range( "a" );
                ASSERT( !frs3.range( "a" ).universal() );

                FieldRangeSet frs4( "", BSON( "a" << NE << 1 ), true );
                FieldRange f4 = frs4.range( "a" );
                ASSERT( !f4.universal() );

                f1 |= f4;
                ASSERT( f1.universal() );
                f1 &= f2;
                ASSERT( !f1.universal() );

                FieldRangeSet frs5( "", BSON( "a" << GT << 1 << LTE << 2 ), true );
                FieldRange f5 = frs5.range( "a" );
                ASSERT( !f5.universal() );
                
                FieldRangeSet frs6( "", BSONObj(), true );
                FieldRange f6 = frs6.range( "a" );
                ASSERT( f6.universal() );
                
                f6 -= f5;
                ASSERT( !f6.universal() );
            }
        };
        
        namespace SimpleFiniteSet {

            class NotSimpleFiniteSet {
            public:
                NotSimpleFiniteSet( const BSONObj &query ) : _query( query ) {}
                virtual ~NotSimpleFiniteSet() {}
                void run() {
                    ASSERT( !FieldRangeSet( "", _query,
                                           !multikey() ).range( "a" ).simpleFiniteSet() );
                }
            protected:
                virtual bool multikey() const { return false; }
            private:
                BSONObj _query;
            };
            
            struct EqualArray : public NotSimpleFiniteSet {
                EqualArray() : NotSimpleFiniteSet( BSON( "a" << BSON_ARRAY( "1" ) ) ) {}
            };
            
            struct EqualEmptyArray : public NotSimpleFiniteSet {
                EqualEmptyArray() : NotSimpleFiniteSet( fromjson( "{a:[]}" ) ) {}
            };
            
            struct InArray : public NotSimpleFiniteSet {
                InArray() : NotSimpleFiniteSet( fromjson( "{a:{$in:[[1]]}}" ) ) {}
            };
            
            struct InRegex : public NotSimpleFiniteSet {
                InRegex() : NotSimpleFiniteSet( fromjson( "{a:{$in:[/^a/]}}" ) ) {}
            };
            
            struct Exists : public NotSimpleFiniteSet {
                Exists() : NotSimpleFiniteSet( fromjson( "{a:{$exists:false}}" ) ) {}
            };

            struct UntypedRegex : public NotSimpleFiniteSet {
                UntypedRegex() : NotSimpleFiniteSet( fromjson( "{a:{$regex:/^a/}}" ) ) {}
            };

            struct NotIn : public NotSimpleFiniteSet {
                NotIn() : NotSimpleFiniteSet( fromjson( "{a:{$not:{$in:[0]}}}" ) ) {}
            };

            /** Descriptive test - behavior could potentially be different. */
            struct NotNe : public NotSimpleFiniteSet {
                NotNe() : NotSimpleFiniteSet( fromjson( "{a:{$not:{$ne:4}}}" ) ) {}
            };
            
            class MultikeyIntersection : public NotSimpleFiniteSet {
            public:
                MultikeyIntersection() : NotSimpleFiniteSet( BSON( "a" << GTE << 0 << "a" << 0 ) ) {
                }
            private:
                virtual bool multikey() const { return true; }                
            };
            
            class Intersection {
            public:
                void run() {
                    FieldRangeSet set( "", BSON( "left" << 1 << "right" << GT << 2 ), true );
                    FieldRange &left = set.range( "left" );
                    FieldRange &right = set.range( "right" );
                    FieldRange &missing = set.range( "missing" );
                    ASSERT( left.simpleFiniteSet() );
                    ASSERT( !right.simpleFiniteSet() );
                    ASSERT( !missing.simpleFiniteSet() );
                    
                    // Replacing a universal range preserves the simpleFiniteSet property.
                    missing &= left;
                    ASSERT( missing.simpleFiniteSet() );
                    
                    // Other operations clear the simpleFiniteSet property.
                    left &= right;
                    ASSERT( !left.simpleFiniteSet() );                    
                }
            };
            
            class Union {
            public:
                void run() {
                    FieldRangeSet set( "", BSON( "left" << 1 << "right" << GT << 2 ), true );
                    FieldRange &left = set.range( "left" );
                    FieldRange &right = set.range( "right" );
                    left |= right;
                    ASSERT( !left.simpleFiniteSet() );
                }
            };

            class Difference {
            public:
                void run() {
                    FieldRangeSet set( "", BSON( "left" << 1 << "right" << GT << 2 ), true );
                    FieldRange &left = set.range( "left" );
                    FieldRange &right = set.range( "right" );
                    left -= right;
                    ASSERT( !left.simpleFiniteSet() );
                }
            };

        } // namespace SimpleFiniteSet
        
    } // namespace FieldRangeTests

    namespace FieldRangeSetTests {

        class ToString {
        public:
            void run() {
                BSONObj obj = BSON( "a" << 1 );
                FieldRangeSet fieldRangeSet( "", obj, true );
                fieldRangeSet.toString(); // Just test that we don't crash.
            }
        };
        
        class Namespace {
        public:
            void run() {
                boost::shared_ptr<FieldRangeSet> frs;
                {
                    string ns = str::stream() << "foo";
                    frs.reset( new FieldRangeSet( ns.c_str(), BSONObj(), true ) );
                }
                ASSERT_EQUALS( string( "foo" ), frs->ns() );
            }
        };

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
                // An intersection with a universal range is allowed.
                frs1 &= frs2;
                ASSERT_EQUALS( frs2.simplifiedQuery( BSONObj() ),
                              frs1.simplifiedQuery( BSONObj() ) );
                // An intersection with non universal range is not allowed, as it might prevent a
                // valid multikey match.
                frs1 &= frs3;
                ASSERT_EQUALS( frs2.simplifiedQuery( BSONObj() ),
                              frs1.simplifiedQuery( BSONObj() ) );
                // Now intersect with a fully contained range.
                FieldRangeSet frs4( "", BSON( "a" << GT << 6 ), false );
                frs1 &= frs4;
                ASSERT_EQUALS( frs4.simplifiedQuery( BSONObj() ),
                              frs1.simplifiedQuery( BSONObj() ) );                
            }
        };

        /* Intersecting an empty multikey range with another range produces an empty range. */
        class EmptyMultiKeyIntersect {
        public:
            void run() {
                FieldRangeSet frs1( "", BSON( "a" << BSON( "$in" << BSONArray() ) ), false );
                FieldRangeSet frs2( "", BSON( "a" << 5 ), false );
                ASSERT( frs1.range( "a" ).empty() );
                frs1 &= frs2;
                ASSERT( frs1.range( "a" ).empty() );
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
        
        class Subset {
        public:
            void run() {
                _frs1.reset
                ( new FieldRangeSet
                 ( "", BSON( "a" << GT << 4 << LT << 4 << "b" << 5 << "c" << 6 ), true ) );
                _frs2.reset( _frs1->subset( BSON( "a" << 1 << "b" << 1 << "d" << 1 ) ) );

                // An empty range should be copied.
                ASSERT( _frs1->range( "a" ).empty() );
                ASSERT( _frs2->range( "a" ).empty() );
                assertRangeCopied( "a" );

                assertRangeCopied( "b" );
                assertRangeNotCopied( "c" );
                assertRangeNotCopied( "d" );
            }
        private:
            void assertRangeCopied( const string &fieldName ) {
                ASSERT_EQUALS( _frs1->range( fieldName.c_str() ).toString(),
                              _frs2->range( fieldName.c_str() ).toString() );
            }
            void assertRangeNotCopied( const string &fieldName ) {
                ASSERT_EQUALS( _frs1->range( "qqqqq" ).toString(), // Missing field, universal range
                              _frs2->range( fieldName.c_str() ).toString() );
            }
            auto_ptr<FieldRangeSet> _frs1;
            auto_ptr<FieldRangeSet> _frs2;
        };
          
        namespace SimpleFiniteSet {

            class SimpleFiniteSet {
            public:
                SimpleFiniteSet( const BSONObj &query ) : _query( query ) {}
                void run() {
                    ASSERT( FieldRangeSet( "", _query, true ).simpleFiniteSet() );
                }
            private:
                BSONObj _query;
            };

            class NotSimpleFiniteSet {
            public:
                NotSimpleFiniteSet( const BSONObj &query ) : _query( query ) {}
                void run() {
                    ASSERT( !FieldRangeSet( "", _query, true ).simpleFiniteSet() );
                }
            private:
                BSONObj _query;
            };

            struct EmptyQuery : public SimpleFiniteSet {
                EmptyQuery() : SimpleFiniteSet( BSONObj() ) {}
            };
            
            struct Equal : public SimpleFiniteSet {
                Equal() : SimpleFiniteSet( BSON( "a" << 0 ) ) {}
            };
            
            struct In : public SimpleFiniteSet {
                In() : SimpleFiniteSet( fromjson( "{a:{$in:[0,1]}}" ) ) {}
            };
            
            struct Where : public NotSimpleFiniteSet {
                Where() : NotSimpleFiniteSet( BSON( "a" << 0 << "$where" << "foo" ) ) {}
            };

            struct Not : public NotSimpleFiniteSet {
                Not() : NotSimpleFiniteSet( fromjson( "{a:{$not:{$in:[0]}}}" ) ) {}
            };

            struct Regex : public NotSimpleFiniteSet {
                Regex() : NotSimpleFiniteSet( fromjson( "{a:/^a/}" ) ) {}
            };
            
            struct UntypedRegex : public NotSimpleFiniteSet {
                UntypedRegex() : NotSimpleFiniteSet( fromjson( "{a:{$regex:'^a'}}" ) ) {}
            };
            
            struct And : public SimpleFiniteSet {
                And() : SimpleFiniteSet( fromjson( "{$and:[{a:{$in:[0,1]}}]}" ) ) {}
            };

            struct All : public NotSimpleFiniteSet {
                All() : NotSimpleFiniteSet( fromjson( "{a:{$all:[0]}}" ) ) {}
            };

            struct ElemMatch : public NotSimpleFiniteSet {
                ElemMatch() : NotSimpleFiniteSet( fromjson( "{a:{$elemMatch:{b:1}}}" ) ) {}
            };

            struct AllElemMatch : public NotSimpleFiniteSet {
                AllElemMatch() :
                        NotSimpleFiniteSet( fromjson( "{a:{$all:[{$elemMatch:{b:1}}]}}" ) ) {}
            };
            
            struct NotSecondField : public NotSimpleFiniteSet {
                NotSecondField() :
                        NotSimpleFiniteSet( fromjson( "{a:{$in:[1],$not:{$in:[0]}}}" ) ) {}
            };
            
        } // namespace SimpleFiniteSet
        
    } // namespace FieldRangeSetTests
    
    namespace FieldRangeSetPairTests {

        class ToString {
        public:
            void run() {
                BSONObj obj = BSON( "a" << 1 );
                FieldRangeSetPair FieldRangeSetPair( "", obj );
                FieldRangeSetPair.toString(); // Just test that we don't crash.
            }
        };

        class NoNonUniversalRanges {
        public:
            void run() {
                FieldRangeSetPair frsp1( "", BSONObj() );
                ASSERT( frsp1.noNonUniversalRanges() );
                FieldRangeSetPair frsp2( "", BSON( "a" << 1 ) );
                ASSERT( !frsp2.noNonUniversalRanges() );
                FieldRangeSetPair frsp3( "", BSON( "a" << GT << 1 ) );
                ASSERT( !frsp3.noNonUniversalRanges() );
                // A single key invalid constraint is not universal.
                FieldRangeSetPair frsp4( "", BSON( "a" << GT << 1 << LT << 0 ) );
                ASSERT( frsp4.frsForIndex( 0, -1 ).matchPossible() );
                ASSERT( !frsp4.noNonUniversalRanges() );
                // Still not universal if multikey invalid constraint.
                FieldRangeSetPair frsp5( "", BSON( "a" << BSON( "$in" << BSONArray() ) ) );
                ASSERT( !frsp5.frsForIndex( 0, -1 ).matchPossible() );
                ASSERT( !frsp5.noNonUniversalRanges() );
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
            Lock::DBWrite _lk;
            Client::Context _ctx;
        public:
            IndexBase() : _lk(ns()), _ctx( ns() ) , indexNum_( 0 ) {
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
                verify( false );
                return 0;
            }
            int indexno( const BSONObj &key ) {
                return nsd()->idxNo( *index(key) );
            }
            static DBDirectClient client_;
        private:
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
        
        /** Check that clearIndexesForPatterns() clears recorded query plans. */
        class ClearIndexesForPatterns : public IndexBase {
        public:
            void run() {
                index( BSON( "a" << 1 ) );
                BSONObj query = BSON( "a" << GT << 5 << LT << 5 );
                BSONObj sort = BSON( "a" << 1 );
                
                // Record the a:1 index for the query's single and multi key query patterns.
                NamespaceDetailsTransient &nsdt = NamespaceDetailsTransient::get( ns() );
                QueryPattern singleKey = FieldRangeSet( ns(), query, true ).pattern( sort );
                nsdt.registerCachedQueryPlanForPattern( singleKey,
                                                       CachedQueryPlan( BSON( "a" << 1 ), 1,
                                                        CandidatePlanCharacter( true, true ) ) );
                QueryPattern multiKey = FieldRangeSet( ns(), query, false ).pattern( sort );
                nsdt.registerCachedQueryPlanForPattern( multiKey,
                                                       CachedQueryPlan( BSON( "a" << 1 ), 5,
                                                        CandidatePlanCharacter( true, true ) ) );
                
                // The single and multi key fields for this query must differ for the test to be
                // valid.
                ASSERT( singleKey != multiKey );
                
                // Clear the recorded query plans using clearIndexesForPatterns.
                FieldRangeSetPair frsp( ns(), query );
                QueryUtilIndexed::clearIndexesForPatterns( frsp, sort );
                
                // Check that the recorded query plans were cleared.
                ASSERT_EQUALS( BSONObj(), nsdt.cachedQueryPlanForPattern( singleKey ).indexKey() );
                ASSERT_EQUALS( BSONObj(), nsdt.cachedQueryPlanForPattern( multiKey ).indexKey() );
            }
        };

        /** Check query plan returned by bestIndexForPatterns(). */
        class BestIndexForPatterns : public IndexBase {
        public:
            void run() {
                index( BSON( "a" << 1 ) );
                index( BSON( "b" << 1 ) );
                BSONObj query = BSON( "a" << GT << 5 << LT << 5 );
                BSONObj sort = BSON( "a" << 1 );
                NamespaceDetailsTransient &nsdt = NamespaceDetailsTransient::get( ns() );

                // No query plan is returned when none has been recorded.
                FieldRangeSetPair frsp( ns(), query );
                ASSERT_EQUALS( BSONObj(),
                              QueryUtilIndexed::bestIndexForPatterns( frsp, sort ).indexKey() );
                
                // A multikey index query plan is returned if recorded.
                QueryPattern multiKey = FieldRangeSet( ns(), query, false ).pattern( sort );
                nsdt.registerCachedQueryPlanForPattern( multiKey,
                                                       CachedQueryPlan( BSON( "a" << 1 ), 5,
                                                        CandidatePlanCharacter( true, true ) ) );
                ASSERT_EQUALS( BSON( "a" << 1 ),
                              QueryUtilIndexed::bestIndexForPatterns( frsp, sort ).indexKey() );

                // A non multikey index query plan is preferentially returned if recorded.
                QueryPattern singleKey = FieldRangeSet( ns(), query, true ).pattern( sort );
                nsdt.registerCachedQueryPlanForPattern( singleKey,
                                                       CachedQueryPlan( BSON( "b" << 1 ), 5,
                                                        CandidatePlanCharacter( true, true ) ) );
                ASSERT_EQUALS( BSON( "b" << 1 ),
                              QueryUtilIndexed::bestIndexForPatterns( frsp, sort ).indexKey() );
                
                // The single and multi key fields for this query must differ for the test to be
                // valid.
                ASSERT( singleKey != multiKey );
            }
        };

    } // namespace FieldRangeSetPairTests
    
    namespace FieldRangeVectorTests {
        class ToString {
        public:
            void run() {
                BSONObj obj = BSON( "a" << 1 );
                FieldRangeSet fieldRangeSet( "", obj, true );
                IndexSpec indexSpec( BSON( "a" << 1 ) );
                FieldRangeVector fieldRangeVector( fieldRangeSet, indexSpec, 1 );
                fieldRangeVector.toString(); // Just test that we don't crash.
            }
        };
    } // namespace FieldRangeVectorTests
    
    // These are currently descriptive, not normative tests.  SERVER-5450
    namespace FieldRangeVectorIteratorTests {
        
        class Base {
        public:
            virtual ~Base() {}
            void run() {
                FieldRangeSet fieldRangeSet( "", query(), true );
                IndexSpec indexSpec( index(), BSONObj() );
                FieldRangeVector fieldRangeVector( fieldRangeSet, indexSpec, 1 );
                _iterator.reset( new FieldRangeVectorIterator( fieldRangeVector,
                                                              singleIntervalLimit() ) );
                _iterator->advance( fieldRangeVector.startKey() );
                _iterator->prepDive();
                check();
            }
        protected:
            virtual BSONObj query() = 0;
            virtual BSONObj index() = 0;
            virtual int singleIntervalLimit() { return 0; }
            virtual void check() = 0;
            void assertAdvanceToNext( const BSONObj &current ) {
                ASSERT_EQUALS( -1, advance( current ) );
            }
            void assertAdvanceTo( const BSONObj &current, const BSONObj &target,
                                 const BSONObj &inclusive = BSONObj() ) {
                int partition = advance( current );
                ASSERT( !iterator().after() );
                BSONObjBuilder advanceToBuilder;
                advanceToBuilder.appendElements( currentPrefix( current, partition ) );
                for( int i = partition; i < (int)iterator().cmp().size(); ++i ) {
                    advanceToBuilder << *iterator().cmp()[ i ];
                }
                assertEqualWithoutFieldNames( target, advanceToBuilder.obj() );
                if ( !inclusive.isEmpty() ) {
                    BSONObjIterator inc( inclusive );
                    for( int i = 0; i < partition; ++i ) inc.next();
                    for( int i = partition; i < (int)iterator().inc().size(); ++i ) {
                        ASSERT_EQUALS( inc.next().Bool(), iterator().inc()[ i ] );
                    }
                }
            }
            void assertAdvanceToAfter( const BSONObj &current, const BSONObj &target ) {
                int partition = advance( current );
                ASSERT( iterator().after() );
                assertEqualWithoutFieldNames( target, currentPrefix( current, partition ) );
            }
            void assertDoneAdvancing( const BSONObj &current ) {
                ASSERT_EQUALS( -2, advance( current ) );                    
            }
        private:
            static bool equalWithoutFieldNames( const BSONObj &one, const BSONObj &two ) {
                return one.woCompare( two, BSONObj(), false ) == 0;
            }
            static void assertEqualWithoutFieldNames( const BSONObj &one, const BSONObj &two ) {
                if ( !equalWithoutFieldNames( one, two ) ) {
                    log() << one << " != " << two << endl;
                    ASSERT( equalWithoutFieldNames( one, two ) );
                }
            }
            BSONObj currentPrefix( const BSONObj &current, int partition ) {
                ASSERT( partition >= 0 );
                BSONObjIterator currentIter( current );
                BSONObjBuilder prefixBuilder;
                for( int i = 0; i < partition; ++i ) {
                    prefixBuilder << currentIter.next();
                }
                return prefixBuilder.obj();                
            }
            FieldRangeVectorIterator &iterator() { return *_iterator; }
            int advance( const BSONObj &current ) {
                return iterator().advance( current );
            }
            scoped_ptr<FieldRangeVectorIterator> _iterator;
        };
        
        class AdvanceToNextIntervalEquality : public Base {
            BSONObj query() { return fromjson( "{a:{$in:[0,1]}}" ); }
            BSONObj index() { return BSON( "a" << 1 ); }
            void check() {
                assertAdvanceToNext( BSON( "a" << 0 ) );
                assertAdvanceTo( BSON( "a" << 0.5 ), BSON( "a" << 1 ) );
            }
        };
        
        class AdvanceToNextIntervalExclusiveInequality : public Base {
            BSONObj query() { return fromjson( "{a:{$in:['a',/^q/,'z']}}" ); }
            BSONObj index() { return BSON( "a" << 1 ); }
            void check() {
                assertAdvanceToNext( BSON( "a" << "a" ) );
                assertAdvanceToNext( BSON( "a" << "q" ) );
                assertAdvanceTo( BSON( "a" << "r" ), BSON( "a" << "z" ) );
                assertAdvanceToNext( BSON( "a" << "z" ) );
            }            
        };

        class AdvanceToNextIntervalEqualityReverse : public Base {
            BSONObj query() { return fromjson( "{a:{$in:[0,1]}}" ); }
            BSONObj index() { return BSON( "a" << -1 ); }
            void check() {
                assertAdvanceToNext( BSON( "a" << 1 ) );
                assertAdvanceTo( BSON( "a" << 0.5 ), BSON( "a" << 0 ) );
            }
        };

        class AdvanceToNextIntervalEqualityCompound : public Base {
            BSONObj query() { return fromjson( "{a:{$in:[0,1]},b:{$in:[4,5]}}" ); }
            BSONObj index() { return BSON( "a" << 1 << "b" << 1 ); }
            void check() {
                assertAdvanceToNext( BSON( "a" << 0 << "b" << 5 ) );
                assertAdvanceToAfter( BSON( "a" << 0 << "b" << 6 ), BSON( "a" << 0 ) );
                assertAdvanceTo( BSON( "a" << 1 << "b" << 2 ), BSON( "a" << 1 << "b" << 4 ) );
                assertAdvanceToNext( BSON( "a" << 1 << "b" << 4 ) );
                assertDoneAdvancing( BSON( "a" << 1 << "b" << 5.1 ) );
            }            
        };

        class AdvanceToNextIntervalIntermediateEqualityCompound : public Base {
            BSONObj query() { return fromjson( "{a:{$in:[0,1]},b:{$in:[4,5]}}" ); }
            BSONObj index() { return BSON( "a" << 1 << "b" << 1 ); }
            void check() {
                assertAdvanceToNext( BSON( "a" << 0 << "b" << 5 ) );
                assertAdvanceTo( BSON( "a" << 0.5 << "b" << 6 ), BSON( "a" << 1 << "b" << 4 ) );
                assertAdvanceToNext( BSON( "a" << 1 << "b" << 4 ) );
            }            
        };

        class AdvanceToNextIntervalIntermediateInMixed : public Base {
            BSONObj query() { return fromjson( "{a:{$in:[0,1]},b:{$in:[/^a/,'c'],$ne:'a'}}" ); }
            BSONObj index() { return BSON( "a" << 1 << "b" << 1 ); }
            void check() {
                assertAdvanceTo( BSON( "a" << 0 << "b" << "bb" ), BSON( "a" << 0 << "b" << "c" ),
                                BSON( "a" << 0 << "b" << true ) );
                assertAdvanceTo( BSON( "a" << 0.5 << "b" << "a" ), BSON( "a" << 1 << "b" << "a" ),
                                BSON( "a" << true << "b" << false ) );
            }            
        };

        class BeforeLowerBound : public Base {
            BSONObj query() { return fromjson( "{a:{$in:[0,1]},b:{$in:[4,5]}}" ); }
            BSONObj index() { return BSON( "a" << 1 << "b" << 1 ); }
            void check() {
                assertAdvanceTo( BSON( "a" << 0 << "b" << 1 ), BSON( "a" << 0 << "b" << 4 ) );
                assertAdvanceToNext( BSON( "a" << 0 << "b" << 4 ) );
            }            
        };
        
        class BeforeLowerBoundMixed : public Base {
            BSONObj query() { return fromjson( "{a:{$in:[0,1]},b:{$in:[/^a/,'c'],$ne:'a'}}" ); }
            BSONObj index() { return BSON( "a" << 1 << "b" << 1 ); }
            void check() {
                assertAdvanceTo( BSON( "a" << 0 << "b" << "" ), BSON( "a" << 0 << "b" << "a" ),
                                BSON( "a" << 0 << "b" << false ) );
                assertAdvanceTo( BSON( "a" << 1 << "b" << "bb" ), BSON( "a" << 1 << "b" << "c" ),
                                BSON( "a" << 0 << "b" << true ) );
            }            
        };
        
        class AdvanceToNextExclusiveIntervalCompound : public Base {
            BSONObj query() { return fromjson( "{a:{$in:['x',/^y/],$ne:'y'},b:{$in:[0,1]}}" ); }
            BSONObj index() { return fromjson( "{a:1,b:1}" ); }
            void check() {
                assertAdvanceToNext( BSON( "a" << "x" << "b" << 1 ) );
                assertAdvanceToAfter( BSON( "a" << "y" << "b" << 0 ), BSON( "a" << "y" ) );
                assertAdvanceToNext( BSON( "a" << "yy" << "b" << 0 ) );
            }
        };
        
        class AdvanceRange : public Base {
            BSONObj query() { return fromjson( "{a:{$gt:2,$lt:8}}" ); }
            BSONObj index() { return fromjson( "{a:1}" ); }
            void check() {
                assertAdvanceToNext( BSON( "a" << 2 ) );
                assertAdvanceToNext( BSON( "a" << 4 ) );
                assertAdvanceToNext( BSON( "a" << 5 ) );
                assertDoneAdvancing( BSON( "a" << 9 ) );
            }
        };
        
        class AdvanceInRange : public Base {
            BSONObj query() { return fromjson( "{a:{$in:[0,1]},b:{$gt:2,$lt:8}}" ); }
            BSONObj index() { return fromjson( "{a:1,b:1}" ); }
            void check() {
                assertAdvanceToNext( BSON( "a" << 0 << "b" << 5 ) );
                assertAdvanceToNext( BSON( "a" << 0 << "b" << 5 ) );
            }
        };
        
        class AdvanceRangeIn : public Base {
            BSONObj query() { return fromjson( "{a:{$gt:2,$lt:8},b:{$in:[0,1]}}" ); }
            BSONObj index() { return fromjson( "{a:1,b:1}" ); }
            void check() {
                assertAdvanceToNext( BSON( "a" << 4 << "b" << 1 ) );
                assertAdvanceToNext( BSON( "a" << 5 << "b" << 0 ) );
            }
        };

        class AdvanceRangeRange : public Base {
            BSONObj query() { return fromjson( "{a:{$gt:2,$lt:8},b:{$gt:1,$lt:4}}" ); }
            BSONObj index() { return fromjson( "{a:1,b:1}" ); }
            void check() {
                assertAdvanceTo( BSON( "a" << 4 << "b" << 0 ), BSON( "a" << 4 << "b" << 1 ) );
                assertAdvanceToAfter( BSON( "a" << 4 << "b" << 6 ), BSON( "a" << 4 ) );
            }
        };

        class AdvanceRangeRangeMultiple : public Base {
            BSONObj query() { return fromjson( "{a:{$gt:2,$lt:8},b:{$gt:1,$lt:4}}" ); }
            BSONObj index() { return fromjson( "{a:1,b:1}" ); }
            void check() {
                assertAdvanceToNext( BSON( "a" << 4 << "b" << 3 ) );
                assertAdvanceToNext( BSON( "a" << 4 << "b" << 3 ) );
            }
        };

        class AdvanceRangeRangeIn : public Base {
            BSONObj query() { return fromjson( "{a:{$gt:2,$lt:8},b:{$gt:1,$lt:4},c:{$in:[6,7]}}" ); }
            BSONObj index() { return fromjson( "{a:1,b:1,c:1}" ); }
            void check() {
                assertAdvanceToNext( BSON( "a" << 4 << "b" << 3 << "c" << 7 ) );
                assertAdvanceToAfter( BSON( "a" << 4 << "b" << 6 << "c" << 7 ), BSON( "a" << 4 ) );
                assertAdvanceToNext( BSON( "a" << 5 << "b" << 3 << "c" << 6 ) );
            }
        };
        
        class AdvanceRangeMixed : public Base {
            BSONObj query() {
                return fromjson( "{a:{$gt:2,$lt:8},b:{$in:['a',/^b/],$ne:'b'}}" );
            }
            BSONObj index() { return fromjson( "{a:1,b:1}" ); }
            void check() {
                assertAdvanceToNext( BSON( "a" << 4 << "b" << "a" ) );
                assertAdvanceToAfter( fromjson( "{a:4,b:'b'}" ), BSON( "a" << 4 << "b" << "b" ) );
            }            
        };

        class AdvanceRangeMixed2 : public Base {
            BSONObj query() {
                return fromjson( "{a:{$gt:2,$lt:8},b:{$in:['a',/^b/],$ne:'b'}}" );
            }
            BSONObj index() { return fromjson( "{a:1,b:1}" ); }
            void check() {
                assertAdvanceToNext( BSON( "a" << 4 << "b" << "a" ) );
                assertAdvanceTo( BSON( "a" << 4 << "b" << "aa" ), BSON( "a" << 4 << "b" << "b" ),
                                BSON( "a" << 0 << "b" << false ) );
            }            
        };

        class AdvanceRangeMixedIn : public Base {
            BSONObj query() {
                return fromjson( "{a:{$gt:2,$lt:8},b:{$in:[/^a/,'c']},c:{$in:[6,7]}}" );
            }
            BSONObj index() { return fromjson( "{a:1,b:1,c:1}" ); }
            void check() {
                assertAdvanceToNext( BSON( "a" << 4 << "b" << "aa" << "c" << 7 ) );
                assertAdvanceToAfter( fromjson( "{a:4,b:/^q/,c:7}" ), BSON( "a" << 4 ) );
                assertAdvanceToNext( BSON( "a" << 5 << "b" << "c" << "c" << 6 ) );
            }
        };

        class AdvanceRangeMixedMixed : public Base {
            BSONObj query() {
                return fromjson( "{a:{$gt:2,$lt:8},b:{$in:['a',/^b/],$ne:'b'},"
                                "c:{$in:[/^a/,'c'],$ne:'a'}}" );
            }
            BSONObj index() { return fromjson( "{a:1,b:1,c:1}" ); }
            void check() {
                assertAdvanceTo( BSON( "a" << 4 << "b" << "a" << "c" << "bb" ),
                                BSON( "a" << 4 << "b" << "a" << "c" << "c" ) );
                assertAdvanceTo( BSON( "a" << 4 << "b" << "aa" << "c" << "b" ),
                                BSON( "a" << 4 << "b" << "b" << "c" << "a" ),
                                BSON( "a" << 0 << "b" << false << "c" << false ) );
            }
        };
        
        class AdvanceMixedMixedIn : public Base {
            BSONObj query() {
                return fromjson( "{a:{$in:[/^a/,'c']},b:{$in:[/^a/,'c']},c:{$in:[6,7]}}" );
            }
            BSONObj index() { return fromjson( "{a:1,b:1,c:1}" ); }
            void check() {
                assertAdvanceToNext( BSON( "a" << "a" << "b" << "aa" << "c" << 7 ) );
                assertAdvanceToAfter( fromjson( "{a:'a',b:/^q/,c:7}" ), BSON( "a" << "a" ) );
                assertAdvanceToNext( BSON( "a" << "c" << "b" << "c" << "c" << 6 ) );
            }
        };

        namespace CompoundRangeCounter {

            class RangeTracking {
            public:
                RangeTracking() : _counter( 2, 0 ) {}
                void run() {
                    ASSERT_EQUALS( 2, _counter.size() );
                    checkValues( -1, -1 );

                    _counter.setZeroes( 0 );
                    checkValues( 0, 0 );

                    _counter.setUnknowns( 1 );
                    checkValues( 0, -1 );

                    _counter.setZeroes( 1 );
                    checkValues( 0, 0 );

                    _counter.setUnknowns( 0 );
                    checkValues( -1, -1 );

                    _counter.set( 0, 3 );
                    checkValues( 3, -1 );

                    _counter.inc( 0 );
                    checkValues( 4, -1 );

                    _counter.set( 1, 5 );
                    checkValues( 4, 5 );

                    _counter.inc( 1 );
                    checkValues( 4, 6 );
                }
            private:
                void checkValues( int first, int second ) {
                    ASSERT_EQUALS( first, _counter.get( 0 ) );
                    ASSERT_EQUALS( second, _counter.get( 1 ) );                
                }
                FieldRangeVectorIterator::CompoundRangeCounter _counter;
            };
            
            class SingleIntervalCount {
            public:
                SingleIntervalCount() : _counter( 2, 2 ) {}
                void run() {
                    assertLimitNotReached();

                    _counter.incSingleIntervalCount();
                    assertLimitNotReached();

                    _counter.incSingleIntervalCount();
                    assertLimitReached();

                    _counter.set( 1, 1 );
                    assertLimitNotReached();
                }
            private:
                void assertLimitReached() const {
                    ASSERT( _counter.hasSingleIntervalCountReachedLimit() );
                }
                void assertLimitNotReached() const {
                    ASSERT( !_counter.hasSingleIntervalCountReachedLimit() );
                }
                FieldRangeVectorIterator::CompoundRangeCounter _counter;
            };
            
            class SingleIntervalCountUpdateBase {
            public:
                SingleIntervalCountUpdateBase() : _counter( 2, 1 ) {}
                virtual ~SingleIntervalCountUpdateBase() {}
                void run() {
                    _counter.incSingleIntervalCount();
                    ASSERT( _counter.hasSingleIntervalCountReachedLimit() );
                    applyUpdate();
                    ASSERT( !_counter.hasSingleIntervalCountReachedLimit() );                    
                }
            protected:
                virtual void applyUpdate() = 0;
                FieldRangeVectorIterator::CompoundRangeCounter &counter() { return _counter; }
            private:
                FieldRangeVectorIterator::CompoundRangeCounter _counter;
            };

            class Set : public SingleIntervalCountUpdateBase {
                void applyUpdate() { counter().set( 0, 1 ); }
            };

            class Inc : public SingleIntervalCountUpdateBase {
                void applyUpdate() { counter().inc( 1 ); }
            };

            class SetZeroes : public SingleIntervalCountUpdateBase {
                void applyUpdate() { counter().setZeroes( 0 ); }
            };

            class SetUnknowns : public SingleIntervalCountUpdateBase {
                void applyUpdate() { counter().setUnknowns( 1 ); }
            };

        } // namespace CompoundRangeCounter

        namespace FieldIntervalMatcher {
            
            class IsEqInclusiveUpperBound {
            public:
                void run() {
                    BSONObj exclusiveInterval = BSON( "$lt" << 5 );
                    for ( int i = 4; i <= 6; ++i ) {
                        for ( int reverse = 0; reverse < 2; ++reverse ) {
                            ASSERT( !isEqInclusiveUpperBound( exclusiveInterval, BSON( "" << i ),
                                                             reverse ) );
                        }
                    }
                    BSONObj inclusiveInterval = BSON( "$lte" << 4 );
                    for ( int i = 3; i <= 5; i += 2 ) {
                        for ( int reverse = 0; reverse < 2; ++reverse ) {
                            ASSERT( !isEqInclusiveUpperBound( inclusiveInterval, BSON( "" << i ),
                                                             reverse ) );
                        }
                    }
                    ASSERT( isEqInclusiveUpperBound( inclusiveInterval, BSON( "" << 4 ), true ) );
                    ASSERT( isEqInclusiveUpperBound( inclusiveInterval, BSON( "" << 4 ), false ) );
                }
            private:
                bool isEqInclusiveUpperBound( const BSONObj &intervalSpec,
                                             const BSONObj &elementSpec,
                                             bool reverse ) {
                    FieldRange range( intervalSpec.firstElement(), true );
                    BSONElement element = elementSpec.firstElement();
                    FieldRangeVectorIterator::FieldIntervalMatcher matcher
                            ( range.intervals()[ 0 ], element, reverse );
                    return matcher.isEqInclusiveUpperBound();
                }
            };
            
            class IsGteUpperBound {
            public:
                void run() {
                    BSONObj exclusiveInterval = BSON( "$lt" << 5 );
                    ASSERT( !isGteUpperBound( exclusiveInterval, BSON( "" << 4 ), false ) );
                    ASSERT( isGteUpperBound( exclusiveInterval, BSON( "" << 5 ), false ) );
                    ASSERT( isGteUpperBound( exclusiveInterval, BSON( "" << 6 ), false ) );
                    ASSERT( isGteUpperBound( exclusiveInterval, BSON( "" << 4 ), true ) );
                    ASSERT( isGteUpperBound( exclusiveInterval, BSON( "" << 5 ), true ) );
                    ASSERT( !isGteUpperBound( exclusiveInterval, BSON( "" << 6 ), true ) );
                    BSONObj inclusiveInterval = BSON( "$lte" << 4 );
                    ASSERT( !isGteUpperBound( inclusiveInterval, BSON( "" << 3 ), false ) );
                    ASSERT( isGteUpperBound( inclusiveInterval, BSON( "" << 4 ), false ) );
                    ASSERT( isGteUpperBound( inclusiveInterval, BSON( "" << 5 ), false ) );
                    ASSERT( isGteUpperBound( inclusiveInterval, BSON( "" << 3 ), true ) );
                    ASSERT( isGteUpperBound( inclusiveInterval, BSON( "" << 4 ), true ) );
                    ASSERT( !isGteUpperBound( inclusiveInterval, BSON( "" << 5 ), true ) );
                }
            private:
                bool isGteUpperBound( const BSONObj &intervalSpec, const BSONObj &elementSpec,
                                     bool reverse ) {
                    FieldRange range( intervalSpec.firstElement(), true );
                    BSONElement element = elementSpec.firstElement();
                    FieldRangeVectorIterator::FieldIntervalMatcher matcher
                            ( range.intervals()[ 0 ], element, reverse );
                    return matcher.isGteUpperBound();
                }
            };

            class IsEqExclusiveLowerBound {
            public:
                void run() {
                    BSONObj exclusiveInterval = BSON( "$gt" << 5 );
                    for ( int i = 4; i <= 6; i += 2 ) {
                        for ( int reverse = 0; reverse < 2; ++reverse ) {
                            ASSERT( !isEqExclusiveLowerBound( exclusiveInterval, BSON( "" << i ),
                                                             reverse ) );
                        }
                    }
                    ASSERT( isEqExclusiveLowerBound( exclusiveInterval, BSON( "" << 5 ), true ) );
                    ASSERT( isEqExclusiveLowerBound( exclusiveInterval, BSON( "" << 5 ), false ) );
                    BSONObj inclusiveInterval = BSON( "$gte" << 4 );
                    for ( int i = 3; i <= 5; ++i ) {
                        for ( int reverse = 0; reverse < 2; ++reverse ) {
                            ASSERT( !isEqExclusiveLowerBound( inclusiveInterval, BSON( "" << i ),
                                                             reverse ) );
                        }
                    }
                }
            private:
                bool isEqExclusiveLowerBound( const BSONObj &intervalSpec,
                                             const BSONObj &elementSpec,
                                             bool reverse ) {
                    FieldRange range( intervalSpec.firstElement(), true );
                    BSONElement element = elementSpec.firstElement();
                    FieldRangeVectorIterator::FieldIntervalMatcher matcher
                            ( range.intervals()[ 0 ], element, reverse );
                    return matcher.isEqExclusiveLowerBound();
                }
            };

            class IsLtLowerBound {
            public:
                void run() {
                    BSONObj exclusiveInterval = BSON( "$gt" << 5 );
                    ASSERT( isLtLowerBound( exclusiveInterval, BSON( "" << 4 ), false ) );
                    ASSERT( !isLtLowerBound( exclusiveInterval, BSON( "" << 5 ), false ) );
                    ASSERT( !isLtLowerBound( exclusiveInterval, BSON( "" << 6 ), false ) );
                    ASSERT( !isLtLowerBound( exclusiveInterval, BSON( "" << 4 ), true ) );
                    ASSERT( !isLtLowerBound( exclusiveInterval, BSON( "" << 5 ), true ) );
                    ASSERT( isLtLowerBound( exclusiveInterval, BSON( "" << 6 ), true ) );
                    BSONObj inclusiveInterval = BSON( "$gte" << 4 );
                    ASSERT( isLtLowerBound( inclusiveInterval, BSON( "" << 3 ), false ) );
                    ASSERT( !isLtLowerBound( inclusiveInterval, BSON( "" << 4 ), false ) );
                    ASSERT( !isLtLowerBound( inclusiveInterval, BSON( "" << 5 ), false ) );
                    ASSERT( !isLtLowerBound( inclusiveInterval, BSON( "" << 3 ), true ) );
                    ASSERT( !isLtLowerBound( inclusiveInterval, BSON( "" << 4 ), true ) );
                    ASSERT( isLtLowerBound( inclusiveInterval, BSON( "" << 5 ), true ) );
                }
            private:
                bool isLtLowerBound( const BSONObj &intervalSpec, const BSONObj &elementSpec,
                                     bool reverse ) {
                    FieldRange range( intervalSpec.firstElement(), true );
                    BSONElement element = elementSpec.firstElement();
                    FieldRangeVectorIterator::FieldIntervalMatcher matcher
                            ( range.intervals()[ 0 ], element, reverse );
                    return matcher.isLtLowerBound();
                }
            };
            
            class CheckLowerAfterUpper {
            public:
                void run() {
                    BSONObj intervalSpec = BSON( "$in" << BSON_ARRAY( 1 << 2 ) );
                    BSONObj elementSpec = BSON( "" << 1 );
                    FieldRange range( intervalSpec.firstElement(), true );
                    BSONElement element = elementSpec.firstElement();
                    FieldRangeVectorIterator::FieldIntervalMatcher matcher
                            ( range.intervals()[ 0 ], element, false );
                    ASSERT( matcher.isEqInclusiveUpperBound() );
                    ASSERT( matcher.isGteUpperBound() );
                    ASSERT( !matcher.isEqExclusiveLowerBound() );
                    ASSERT( !matcher.isLtLowerBound() );
                }
            };

        } // namespace FieldIntervalMatcher

        namespace SingleIntervalLimit {

            class NoLimit : public Base {
                BSONObj query() { return BSON( "a" << 1 ); }
                BSONObj index() { return BSON( "a" << 1 ); }
                int singleIntervalLimit() { return 0; }
                void check() {
                    for( int i = 0; i < 100; ++i ) {
                        assertAdvanceToNext( BSON( "a" << 1 ) );
                    }
                }
            };

            class OneIntervalLimit : public Base {
                BSONObj query() { return BSON( "a" << 1 ); }
                BSONObj index() { return BSON( "a" << 1 ); }
                int singleIntervalLimit() { return 3; }
                void check() {
                    for( int i = 0; i < 3; ++i ) {
                        assertAdvanceToNext( BSON( "a" << 1 ) );
                    }
                    assertDoneAdvancing( BSON( "a" << 1 ) );
                }
            };

            class TwoIntervalLimit : public Base {
                BSONObj query() { return fromjson( "{a:{$in:[0,1]}}" ); }
                BSONObj index() { return BSON( "a" << 1 ); }
                int singleIntervalLimit() { return 2; }
                void check() {
                    for( int i = 0; i < 2; ++i ) {
                        assertAdvanceToNext( BSON( "a" << 0 ) );
                    }
                    assertAdvanceTo( BSON( "a" << 0 ), BSON( "a" << 1 ) );
                    for( int i = 0; i < 2; ++i ) {
                        assertAdvanceToNext( BSON( "a" << 1 ) );
                    }
                    assertDoneAdvancing( BSON( "a" << 1 ) );
                }
            };

            class ThreeIntervalLimitUnreached : public Base {
                BSONObj query() { return fromjson( "{a:{$in:[0,1,2]}}" ); }
                BSONObj index() { return BSON( "a" << 1 ); }
                int singleIntervalLimit() { return 2; }
                void check() {
                    for( int i = 0; i < 2; ++i ) {
                        assertAdvanceToNext( BSON( "a" << 0 ) );
                    }
                    assertAdvanceTo( BSON( "a" << 1.5 ), BSON( "a" << 2 ) );
                }
            };

            class FirstIntervalExhaustedBeforeLimit : public Base {
                BSONObj query() { return fromjson( "{a:{$in:[0,1]}}" ); }
                BSONObj index() { return BSON( "a" << 1 ); }
                int singleIntervalLimit() { return 3; }
                void check() {
                    assertAdvanceToNext( BSON( "a" << 0 ) );
                    for( int i = 0; i < 3; ++i ) {
                        assertAdvanceToNext( BSON( "a" << 1 ) );
                    }
                    assertDoneAdvancing( BSON( "a" << 1 ) );
                }
            };

            class FirstIntervalNotExhaustedAtLimit : public Base {
                BSONObj query() { return fromjson( "{a:{$in:[0,1]}}" ); }
                BSONObj index() { return BSON( "a" << 1 ); }
                int singleIntervalLimit() { return 3; }
                void check() {
                    for( int i = 0; i < 3; ++i ) {
                        assertAdvanceToNext( BSON( "a" << 0 ) );
                    }
                    for( int i = 0; i < 3; ++i ) {
                        assertAdvanceToNext( BSON( "a" << 1 ) );
                    }
                    assertDoneAdvancing( BSON( "a" << 1 ) );
                }
            };
            
            class EqualIn : public Base {
                BSONObj query() { return fromjson( "{a:1,b:{$in:[0,1]}}" ); }
                BSONObj index() { return BSON( "a" << 1 << "b" << 1 ); }
                int singleIntervalLimit() { return 3; }
                void check() {
                    for( int i = 0; i < 3; ++i ) {
                        assertAdvanceToNext( BSON( "a" << 1 << "b" << 0 ) );
                    }
                    assertAdvanceTo( BSON( "a" << 1 << "b" << 0 ), BSON( "a" << 1 << "b" << 1 ) );
                    for( int i = 0; i < 3; ++i ) {
                        assertAdvanceToNext( BSON( "a" << 1 << "b" << 1 ) );
                    }
                    assertDoneAdvancing( BSON( "a" << 1 << "b" << 1 ) );
                }
            };
            
            class TwoIntervalIntermediateValue : public Base {
                BSONObj query() { return fromjson( "{a:{$in:[0,1]}}" ); }
                BSONObj index() { return BSON( "a" << 1 ); }
                int singleIntervalLimit() { return 3; }
                void check() {
                    for( int i = 0; i < 2; ++i ) {
                        assertAdvanceToNext( BSON( "a" << 0 ) );
                    }
                    assertAdvanceTo( BSON( "a" << 0.5 ), BSON( "a" << 1 ) );
                    for( int i = 0; i < 3; ++i ) {
                        assertAdvanceToNext( BSON( "a" << 1 ) );
                    }
                    assertDoneAdvancing( BSON( "a" << 1 ) );
                }
            };

            /**
             * The singleIntervalLimit feature should not be used with range bounds, in spite of
             * the corner case checked in this test.
             */
            class TwoRange : public Base {
                // The singleIntervalLimit feature should not 
                BSONObj query() { return fromjson( "{a:{$in:[/^a/,/^c/]}}" ); }
                BSONObj index() { return BSON( "a" << 1 ); }
                int singleIntervalLimit() { return 2; }
                void check() {
                    for( int i = 0; i < 2; ++i ) {
                        assertAdvanceToNext( BSON( "a" << "a" ) );
                    }
                    assertAdvanceTo( BSON( "a" << "b" ), BSON( "a" << "c" ) );
                }
            };

        } // namespace SameRangeLimit
        
    } // namespace FieldRangeVectorIteratorTests
    
    class All : public Suite {
    public:
        All() : Suite( "queryutil" ) {}

        void setupTests() {
            add<FieldIntervalTests::ToString>();
            add<FieldRangeTests::ToString>();
            add<FieldRangeTests::EmptyQuery>();
            add<FieldRangeTests::Eq>();
            add<FieldRangeTests::DupEq>();
            add<FieldRangeTests::Lt>();
            add<FieldRangeTests::Lte>();
            add<FieldRangeTests::Gt>();
            add<FieldRangeTests::Gte>();
            add<FieldRangeTests::TwoLt>();
            add<FieldRangeTests::TwoGt>();
            add<FieldRangeTests::EqGte>();
            add<FieldRangeTests::EqGteInvalid>();
            add<FieldRangeTests::Regex>();
            add<FieldRangeTests::RegexObj>();
            add<FieldRangeTests::UnhelpfulRegex>();
            add<FieldRangeTests::In>();
            add<FieldRangeTests::And>();
            add<FieldRangeTests::Empty>();
            add<FieldRangeTests::Equality>();
            add<FieldRangeTests::SimplifiedQuery>();
            add<FieldRangeTests::QueryPatternTest>();
            add<FieldRangeTests::QueryPatternEmpty>();
            add<FieldRangeTests::QueryPatternNeConstraint>();
            add<FieldRangeTests::QueryPatternOptimizedBounds>();
            add<FieldRangeTests::NoWhere>();
            add<FieldRangeTests::Numeric>();
            add<FieldRangeTests::InLowerBound>();
            add<FieldRangeTests::InUpperBound>();
            add<FieldRangeTests::BoundUnion>();
            add<FieldRangeTests::BoundUnionFullyContained>();
            add<FieldRangeTests::BoundUnionOverlapWithInclusivity>();
            add<FieldRangeTests::BoundUnionEmpty>();
            add<FieldRangeTests::MultiBound>();
            add<FieldRangeTests::Diff1>();
            add<FieldRangeTests::Diff2>();
            add<FieldRangeTests::Diff3>();
            add<FieldRangeTests::Diff4>();
            add<FieldRangeTests::Diff5>();
            add<FieldRangeTests::Diff6>();
            add<FieldRangeTests::Diff7>();
            add<FieldRangeTests::Diff8>();
            add<FieldRangeTests::Diff9>();
            add<FieldRangeTests::Diff10>();
            add<FieldRangeTests::Diff11>();
            add<FieldRangeTests::Diff12>();
            add<FieldRangeTests::Diff13>();
            add<FieldRangeTests::Diff14>();
            add<FieldRangeTests::Diff15>();
            add<FieldRangeTests::Diff16>();
            add<FieldRangeTests::Diff17>();
            add<FieldRangeTests::Diff18>();
            add<FieldRangeTests::Diff19>();
            add<FieldRangeTests::Diff20>();
            add<FieldRangeTests::Diff21>();
            add<FieldRangeTests::Diff22>();
            add<FieldRangeTests::Diff23>();
            add<FieldRangeTests::Diff24>();
            add<FieldRangeTests::Diff25>();
            add<FieldRangeTests::Diff26>();
            add<FieldRangeTests::Diff27>();
            add<FieldRangeTests::Diff28>();
            add<FieldRangeTests::Diff29>();
            add<FieldRangeTests::Diff30>();
            add<FieldRangeTests::Diff31>();
            add<FieldRangeTests::Diff32>();
            add<FieldRangeTests::Diff33>();
            add<FieldRangeTests::Diff34>();
            add<FieldRangeTests::Diff35>();
            add<FieldRangeTests::Diff36>();
            add<FieldRangeTests::Diff37>();
            add<FieldRangeTests::Diff38>();
            add<FieldRangeTests::Diff39>();
            add<FieldRangeTests::Diff40>();
            add<FieldRangeTests::Diff41>();
            add<FieldRangeTests::Diff42>();
            add<FieldRangeTests::Diff43>();
            add<FieldRangeTests::Diff44>();
            add<FieldRangeTests::Diff45>();
            add<FieldRangeTests::Diff46>();
            add<FieldRangeTests::Diff47>();
            add<FieldRangeTests::Diff48>();
            add<FieldRangeTests::Diff49>();
            add<FieldRangeTests::Diff50>();
            add<FieldRangeTests::Diff51>();
            add<FieldRangeTests::Diff52>();
            add<FieldRangeTests::Diff53>();
            add<FieldRangeTests::Diff54>();
            add<FieldRangeTests::Diff55>();
            add<FieldRangeTests::Diff56>();
            add<FieldRangeTests::Diff57>();
            add<FieldRangeTests::Diff58>();
            add<FieldRangeTests::Diff59>();
            add<FieldRangeTests::Diff60>();
            add<FieldRangeTests::Diff61>();
            add<FieldRangeTests::Diff62>();
            add<FieldRangeTests::Diff63>();
            add<FieldRangeTests::Diff64>();
            add<FieldRangeTests::DiffMulti1>();
            add<FieldRangeTests::DiffMulti2>();
            add<FieldRangeTests::Universal>();
            add<FieldRangeTests::SimpleFiniteSet::EqualArray>();
            add<FieldRangeTests::SimpleFiniteSet::EqualEmptyArray>();
            add<FieldRangeTests::SimpleFiniteSet::InArray>();
            add<FieldRangeTests::SimpleFiniteSet::InRegex>();
            add<FieldRangeTests::SimpleFiniteSet::Exists>();
            add<FieldRangeTests::SimpleFiniteSet::UntypedRegex>();
            add<FieldRangeTests::SimpleFiniteSet::NotIn>();
            add<FieldRangeTests::SimpleFiniteSet::NotNe>();
            add<FieldRangeTests::SimpleFiniteSet::MultikeyIntersection>();
            add<FieldRangeTests::SimpleFiniteSet::Intersection>();
            add<FieldRangeTests::SimpleFiniteSet::Union>();
            add<FieldRangeTests::SimpleFiniteSet::Difference>();
            add<FieldRangeSetTests::ToString>();
            add<FieldRangeSetTests::Namespace>();
            add<FieldRangeSetTests::Intersect>();
            add<FieldRangeSetTests::MultiKeyIntersect>();
            add<FieldRangeSetTests::EmptyMultiKeyIntersect>();
            add<FieldRangeSetTests::MultiKeyDiff>();
            add<FieldRangeSetTests::MatchPossible>();
            add<FieldRangeSetTests::MatchPossibleForIndex>();
            add<FieldRangeSetTests::Subset>();
            add<FieldRangeSetTests::SimpleFiniteSet::EmptyQuery>();
            add<FieldRangeSetTests::SimpleFiniteSet::Equal>();
            add<FieldRangeSetTests::SimpleFiniteSet::In>();
            add<FieldRangeSetTests::SimpleFiniteSet::Where>();
            add<FieldRangeSetTests::SimpleFiniteSet::Not>();
            add<FieldRangeSetTests::SimpleFiniteSet::Regex>();
            add<FieldRangeSetTests::SimpleFiniteSet::UntypedRegex>();
            add<FieldRangeSetTests::SimpleFiniteSet::And>();
            add<FieldRangeSetTests::SimpleFiniteSet::All>();
            add<FieldRangeSetTests::SimpleFiniteSet::ElemMatch>();
            add<FieldRangeSetTests::SimpleFiniteSet::AllElemMatch>();
            add<FieldRangeSetTests::SimpleFiniteSet::NotSecondField>();
            add<FieldRangeSetPairTests::ToString>();
            add<FieldRangeSetPairTests::NoNonUniversalRanges>();
            add<FieldRangeSetPairTests::MatchPossible>();
            add<FieldRangeSetPairTests::MatchPossibleForIndex>();
            add<FieldRangeSetPairTests::ClearIndexesForPatterns>();
            add<FieldRangeSetPairTests::BestIndexForPatterns>();
            add<FieldRangeVectorTests::ToString>();
            add<FieldRangeVectorIteratorTests::AdvanceToNextIntervalEquality>();
            add<FieldRangeVectorIteratorTests::AdvanceToNextIntervalExclusiveInequality>();
            add<FieldRangeVectorIteratorTests::AdvanceToNextIntervalEqualityReverse>();
            add<FieldRangeVectorIteratorTests::AdvanceToNextIntervalEqualityCompound>();
            add<FieldRangeVectorIteratorTests::AdvanceToNextIntervalIntermediateEqualityCompound>();
            add<FieldRangeVectorIteratorTests::AdvanceToNextIntervalIntermediateInMixed>();
            add<FieldRangeVectorIteratorTests::BeforeLowerBound>();
            add<FieldRangeVectorIteratorTests::BeforeLowerBoundMixed>();
            add<FieldRangeVectorIteratorTests::AdvanceToNextExclusiveIntervalCompound>();
            add<FieldRangeVectorIteratorTests::AdvanceRange>();
            add<FieldRangeVectorIteratorTests::AdvanceInRange>();
            add<FieldRangeVectorIteratorTests::AdvanceRangeIn>();
            add<FieldRangeVectorIteratorTests::AdvanceRangeRange>();
            add<FieldRangeVectorIteratorTests::AdvanceRangeRangeMultiple>();
            add<FieldRangeVectorIteratorTests::AdvanceRangeRangeIn>();
            add<FieldRangeVectorIteratorTests::AdvanceRangeMixed>();
            add<FieldRangeVectorIteratorTests::AdvanceRangeMixed2>();
            add<FieldRangeVectorIteratorTests::AdvanceRangeMixedIn>();
            add<FieldRangeVectorIteratorTests::AdvanceRangeMixedMixed>();
            add<FieldRangeVectorIteratorTests::AdvanceMixedMixedIn>();
            add<FieldRangeVectorIteratorTests::CompoundRangeCounter::RangeTracking>();
            add<FieldRangeVectorIteratorTests::CompoundRangeCounter::SingleIntervalCount>();
            add<FieldRangeVectorIteratorTests::CompoundRangeCounter::Set>();
            add<FieldRangeVectorIteratorTests::CompoundRangeCounter::Inc>();
            add<FieldRangeVectorIteratorTests::CompoundRangeCounter::SetZeroes>();
            add<FieldRangeVectorIteratorTests::CompoundRangeCounter::SetUnknowns>();
            add<FieldRangeVectorIteratorTests::FieldIntervalMatcher::IsEqInclusiveUpperBound>();
            add<FieldRangeVectorIteratorTests::FieldIntervalMatcher::IsGteUpperBound>();
            add<FieldRangeVectorIteratorTests::FieldIntervalMatcher::IsEqExclusiveLowerBound>();
            add<FieldRangeVectorIteratorTests::FieldIntervalMatcher::IsLtLowerBound>();
            add<FieldRangeVectorIteratorTests::FieldIntervalMatcher::CheckLowerAfterUpper>();
            add<FieldRangeVectorIteratorTests::SingleIntervalLimit::NoLimit>();
            add<FieldRangeVectorIteratorTests::SingleIntervalLimit::OneIntervalLimit>();
            add<FieldRangeVectorIteratorTests::SingleIntervalLimit::TwoIntervalLimit>();
            add<FieldRangeVectorIteratorTests::SingleIntervalLimit::ThreeIntervalLimitUnreached>();
            add<FieldRangeVectorIteratorTests::SingleIntervalLimit::
                    FirstIntervalExhaustedBeforeLimit>();
            add<FieldRangeVectorIteratorTests::SingleIntervalLimit::
                    FirstIntervalNotExhaustedAtLimit>();
            add<FieldRangeVectorIteratorTests::SingleIntervalLimit::EqualIn>();
            add<FieldRangeVectorIteratorTests::SingleIntervalLimit::TwoIntervalIntermediateValue>();
            add<FieldRangeVectorIteratorTests::SingleIntervalLimit::TwoRange>();
        }
    } myall;

} // namespace QueryUtilTests

