// matchertests.cpp : matcher unit tests
//

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

#include "pch.h"

#include "mongo/db/matcher.h"

#include "mongo/db/cursor.h"
#include "mongo/db/json.h"
#include "mongo/db/namespace_details.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/util/timer.h"

namespace MatcherTests {

    class CollectionBase {
    public:
        CollectionBase() :
        _ns( "unittests.matchertests" ) {
        }
        virtual ~CollectionBase() {
            client().dropCollection( ns() );
        }
    protected:
        const char * const ns() const { return _ns; }
        DBDirectClient &client() { return _client; }
    private:
        const char * const _ns;
        DBDirectClient _client;
    };
    
    class Basic {
    public:
        void run() {
            BSONObj query = fromjson( "{\"a\":\"b\"}" );
            Matcher m( query );
            ASSERT( m.matches( fromjson( "{\"a\":\"b\"}" ) ) );
        }
    };

    class DoubleEqual {
    public:
        void run() {
            BSONObj query = fromjson( "{\"a\":5}" );
            Matcher m( query );
            ASSERT( m.matches( fromjson( "{\"a\":5}" ) ) );
        }
    };

    class MixedNumericEqual {
    public:
        void run() {
            BSONObjBuilder query;
            query.append( "a", 5 );
            Matcher m( query.done() );
            ASSERT( m.matches( fromjson( "{\"a\":5}" ) ) );
        }
    };

    class MixedNumericGt {
    public:
        void run() {
            BSONObj query = fromjson( "{\"a\":{\"$gt\":4}}" );
            Matcher m( query );
            BSONObjBuilder b;
            b.append( "a", 5 );
            ASSERT( m.matches( b.done() ) );
        }
    };

    class MixedNumericIN {
    public:
        void run() {
            BSONObj query = fromjson( "{ a : { $in : [4,6] } }" );
            ASSERT_EQUALS( 4 , query["a"].embeddedObject()["$in"].embeddedObject()["0"].number() );
            ASSERT_EQUALS( NumberInt , query["a"].embeddedObject()["$in"].embeddedObject()["0"].type() );

            Matcher m( query );

            {
                BSONObjBuilder b;
                b.append( "a" , 4.0 );
                ASSERT( m.matches( b.done() ) );
            }

            {
                BSONObjBuilder b;
                b.append( "a" , 5 );
                ASSERT( ! m.matches( b.done() ) );
            }


            {
                BSONObjBuilder b;
                b.append( "a" , 4 );
                ASSERT( m.matches( b.done() ) );
            }

        }
    };

    class MixedNumericEmbedded {
    public:
        void run() {
            Matcher m( BSON( "a" << BSON( "x" << 1 ) ) );
            ASSERT( m.matches( BSON( "a" << BSON( "x" << 1 ) ) ) );
            ASSERT( m.matches( BSON( "a" << BSON( "x" << 1.0 ) ) ) );
        }
    };

    class Size {
    public:
        void run() {
            Matcher m( fromjson( "{a:{$size:4}}" ) );
            ASSERT( m.matches( fromjson( "{a:[1,2,3,4]}" ) ) );
            ASSERT( !m.matches( fromjson( "{a:[1,2,3]}" ) ) );
            ASSERT( !m.matches( fromjson( "{a:[1,2,3,'a','b']}" ) ) );
            ASSERT( !m.matches( fromjson( "{a:[[1,2,3,4]]}" ) ) );
        }
    };

    class WithinBox {
    public:
        void run() {
            Matcher m(fromjson("{loc:{$within:{$box:[{x: 4, y:4},[6,6]]}}}"));
            ASSERT(!m.matches(fromjson("{loc: [3,4]}")));
            ASSERT(m.matches(fromjson("{loc: [4,4]}")));
            ASSERT(m.matches(fromjson("{loc: [5,5]}")));
            ASSERT(m.matches(fromjson("{loc: [5,5.1]}")));
            ASSERT(m.matches(fromjson("{loc: {x: 5, y:5.1}}")));
        }
    };

    class WithinPolygon {
    public:
        void run() {
            Matcher m(fromjson("{loc:{$within:{$polygon:[{x:0,y:0},[0,5],[5,5],[5,0]]}}}"));
            ASSERT(m.matches(fromjson("{loc: [3,4]}")));
            ASSERT(m.matches(fromjson("{loc: [4,4]}")));
            ASSERT(m.matches(fromjson("{loc: {x:5,y:5}}")));
            ASSERT(!m.matches(fromjson("{loc: [5,5.1]}")));
            ASSERT(!m.matches(fromjson("{loc: {}}")));
        }
    };

    class WithinCenter {
    public:
        void run() {
            Matcher m(fromjson("{loc:{$within:{$center:[{x:30,y:30},10]}}}"));
            ASSERT(!m.matches(fromjson("{loc: [3,4]}")));
            ASSERT(m.matches(fromjson("{loc: {x:30,y:30}}")));
            ASSERT(m.matches(fromjson("{loc: [20,30]}")));
            ASSERT(m.matches(fromjson("{loc: [30,20]}")));
            ASSERT(m.matches(fromjson("{loc: [40,30]}")));
            ASSERT(m.matches(fromjson("{loc: [30,40]}")));
            ASSERT(!m.matches(fromjson("{loc: [31,40]}")));
        }
    };

    /** Test that MatchDetails::elemMatchKey() is set correctly after a match. */
    class ElemMatchKey {
    public:
        void run() {
            Matcher matcher( BSON( "a.b" << 1 ) );
            MatchDetails details;
            details.requestElemMatchKey();
            ASSERT( !details.hasElemMatchKey() );
            ASSERT( matcher.matches( fromjson( "{ a:[ { b:1 } ] }" ), &details ) );
            // The '0' entry of the 'a' array is matched.
            ASSERT( details.hasElemMatchKey() );
            ASSERT_EQUALS( string( "0" ), details.elemMatchKey() );
        }
    };

    namespace Covered { // Tests for CoveredIndexMatcher.
    
        /**
         * Test that MatchDetails::elemMatchKey() is set correctly after an unindexed cursor match.
         */
        class ElemMatchKeyUnindexed : public CollectionBase {
        public:
            void run() {
                client().insert( ns(), fromjson( "{ a:[ {}, { b:1 } ] }" ) );
                
                Client::ReadContext context( ns() );

                CoveredIndexMatcher matcher( BSON( "a.b" << 1 ), BSON( "$natural" << 1 ) );
                MatchDetails details;
                details.requestElemMatchKey();
                boost::shared_ptr<Cursor> cursor = NamespaceDetailsTransient::getCursor( ns(), BSONObj() );
                // Verify that the cursor is unindexed.
                ASSERT_EQUALS( "BasicCursor", cursor->toString() );
                ASSERT( matcher.matchesCurrent( cursor.get(), &details ) );
                // The '1' entry of the 'a' array is matched.
                ASSERT( details.hasElemMatchKey() );
                ASSERT_EQUALS( string( "1" ), details.elemMatchKey() );
            }
        };
        
        /**
         * Test that MatchDetails::elemMatchKey() is set correctly after an indexed cursor match.
         */
        class ElemMatchKeyIndexed : public CollectionBase {
        public:
            void run() {
                client().ensureIndex( ns(), BSON( "a.b" << 1 ) );
                client().insert( ns(), fromjson( "{ a:[ {}, { b:9 }, { b:1 } ] }" ) );
                
                Client::ReadContext context( ns() );
                
                BSONObj query = BSON( "a.b" << 1 );
                CoveredIndexMatcher matcher( query, BSON( "a.b" << 1 ) );
                MatchDetails details;
                details.requestElemMatchKey();
                boost::shared_ptr<Cursor> cursor = NamespaceDetailsTransient::getCursor( ns(), query );
                // Verify that the cursor is indexed.
                ASSERT_EQUALS( "BtreeCursor a.b_1", cursor->toString() );
                ASSERT( matcher.matchesCurrent( cursor.get(), &details ) );
                // The '2' entry of the 'a' array is matched.
                ASSERT( details.hasElemMatchKey() );
                ASSERT_EQUALS( string( "2" ), details.elemMatchKey() );
            }
        };
        
        /**
         * Test that MatchDetails::elemMatchKey() is set correctly after an indexed cursor match
         * on a non multikey index.
         */
        class ElemMatchKeyIndexedSingleKey : public CollectionBase {
        public:
            void run() {
                client().ensureIndex( ns(), BSON( "a.b" << 1 ) );
                client().insert( ns(), fromjson( "{ a:[ { b:1 } ] }" ) );
                
                Client::ReadContext context( ns() );
                
                BSONObj query = BSON( "a.b" << 1 );
                CoveredIndexMatcher matcher( query, BSON( "a.b" << 1 ) );
                MatchDetails details;
                details.requestElemMatchKey();
                boost::shared_ptr<Cursor> cursor = NamespaceDetailsTransient::getCursor( ns(), query );
                // Verify that the cursor is indexed.
                ASSERT_EQUALS( "BtreeCursor a.b_1", cursor->toString() );
                // Verify that the cursor is not multikey.
                ASSERT( !cursor->isMultiKey() );
                ASSERT( matcher.matchesCurrent( cursor.get(), &details ) );
                // The '0' entry of the 'a' array is matched.
                ASSERT( details.hasElemMatchKey() );
                ASSERT_EQUALS( string( "0" ), details.elemMatchKey() );
            }
        };
        
    } // namespace Covered
    
    class TimingBase {
    public:
        long time( const BSONObj& patt , const BSONObj& obj ) {
            Matcher m( patt );
            Timer t;
            for ( int i=0; i<10000; i++ ) {
                ASSERT( m.matches( obj ) );
            }
            return t.millis();
        }
    };

    class AllTiming : public TimingBase {
    public:
        void run() {
            long normal = time( BSON( "x" << 5 ) , BSON( "x" << 5 ) );
            long all = time( BSON( "x" << BSON( "$all" << BSON_ARRAY( 5 ) ) ) , BSON( "x" << 5 ) );

            cout << "normal: " << normal << " all: " << all << endl;
        }
    };

    /**
     * Helper class to extract the top level equality fields of a matcher, which can serve as a
     * useful way to identify the matcher.
     */
    class EqualityFieldExtractor : public MatcherVisitor {
    public:
        EqualityFieldExtractor( const Matcher &originalMatcher ) :
            _originalMatcher( &originalMatcher ),
            _currentMatcher( 0 ) {
        }
        virtual void visitMatcher( const Matcher &matcher ) {
            _currentMatcher = &matcher;
        }
        virtual void visitElementMatcher( const ElementMatcher &elementMatcher ) {
            // If elementMatcher is visited before any Matcher other than _originalMatcher, it is
            // a top level ElementMatcher within _originalMatcher.
            if ( _currentMatcher != _originalMatcher ) {
                return;
            }
            if ( elementMatcher._compareOp != BSONObj::Equality ) {
                return;
            }
            _equalityFields.insert( elementMatcher._toMatch.fieldName() );
        }
        BSONArray equalityFields() const {
            BSONArrayBuilder ret;
            for( set<string>::const_iterator i = _equalityFields.begin();
                i != _equalityFields.end(); ++i ) {
                ret << *i;
            }
            return ret.arr();
        }
        const Matcher *_originalMatcher;
        const Matcher *_currentMatcher;
        set<string> _equalityFields;
    };

    /**
     * Matcher::visit() visits all nested Matchers and ElementMatchers, in the expected
     * order.  In particular:
     * - All of a Matcher's top level ElementMatchers are visited immediately after the Matcher
     *   itself (before any other Matchers are visited).
     * - All nested Matchers and ElementMatchers are visited.
     */
    class Visit {
    public:
        void run() {
            Matcher matcher( fromjson( "{ a:1, b:2, $and:[ { c:6, d:7 }, { n:12 } ],"
                                      "$or:[ { e:8, l:10 } ], $nor:[ { f:9, m:11 } ],"
                                      "g:{ $elemMatch:{ h:3 } },"
                                      "i:{ $all:[ { $elemMatch:{ j:4 } },"
                                      "{ $elemMatch:{ k:5 } } ] } }" ) );
            Visitor testVisitor;
            matcher.visit( testVisitor );
            BSONObj expectedTraversal = fromjson
                    ( "{"
                     "Matcher:[ 'a', 'b' ],"
                     "ElementMatcher:{ a:1 },"
                     "ElementMatcher:{ b:2 },"
                     "ElementMatcher:{ g:{ h:3 } },"
                     "ElementMatcher:{ i:{ $all:[ { $elemMatch:{ j:4 } },"
                            "{ $elemMatch:{ k:5 } } ] } },"
                     "Matcher:[ 'h' ],"
                     "ElementMatcher:{ h:3 },"
                     "Matcher:[ 'j' ],"
                     "ElementMatcher:{ j:4 },"
                     "Matcher:[ 'k' ],"
                     "ElementMatcher:{ k:5 },"
                     "Matcher:[ 'c', 'd' ],"
                     "ElementMatcher:{ c:6 },"
                     "ElementMatcher:{ d:7 },"
                     "Matcher:[ 'n' ],"
                     "ElementMatcher:{ n:12 },"
                     "Matcher:[ 'e', 'l' ],"
                     "ElementMatcher:{ e:8 },"
                     "ElementMatcher:{ l:10 },"
                     "Matcher:[ 'f', 'm' ],"
                     "ElementMatcher:{ f:9 },"
                     "ElementMatcher:{ m:11 }"
                     "}" );
            ASSERT_EQUALS( expectedTraversal, testVisitor.traversal() );
        }
    private:
        /** Helper MatcherVisitor class that records all visit callbacks. */
        class Visitor : public MatcherVisitor {
        public:
            virtual void visitMatcher( const Matcher &matcher ) {
                _traversal << "Matcher" << extractEqualityFields( matcher );
            }
            virtual void visitElementMatcher( const ElementMatcher &elementMatcher ) {
                _traversal << "ElementMatcher" << elementMatcher._toMatch.wrap();
            }
            BSONObj traversal() { return _traversal.obj(); }
        private:
            static BSONArray extractEqualityFields( const Matcher &matcher ) {
                EqualityFieldExtractor extractor( matcher );
                matcher.visit( extractor );
                return extractor.equalityFields();                    
            }
            BSONObjBuilder _traversal;
        };
    };
    
    class All : public Suite {
    public:
        All() : Suite( "matcher" ) {
        }

        void setupTests() {
            add<Basic>();
            add<DoubleEqual>();
            add<MixedNumericEqual>();
            add<MixedNumericGt>();
            add<MixedNumericIN>();
            add<Size>();
            add<MixedNumericEmbedded>();
            add<ElemMatchKey>();
            add<Covered::ElemMatchKeyUnindexed>();
            add<Covered::ElemMatchKeyIndexed>();
            add<Covered::ElemMatchKeyIndexedSingleKey>();
            add<AllTiming>();
            add<Visit>();
            add<WithinBox>();
            add<WithinCenter>();
            add<WithinPolygon>();
        }
    } dball;

} // namespace MatcherTests

