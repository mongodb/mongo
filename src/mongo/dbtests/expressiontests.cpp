// expressiontests.cpp : Unit tests for Expression classes.

/**
 *    Copyright (C) 2012 10gen Inc.
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

#include "mongo/db/pipeline/expression.h"

#include "mongo/db/pipeline/document.h"

#include "dbtests.h"

namespace ExpressionTests {

    /** Check binary equality, ensuring use of the same numeric types. */
    static void assertBinaryEqual( const BSONObj& expected, const BSONObj& actual ) {
        ASSERT_EQUALS( expected, actual );
        ASSERT( expected.binaryEqual( actual ) );
    }

    /** Convert Value to a wrapped BSONObj with an empty string field name. */
    static BSONObj toBson( const intrusive_ptr<const Value>& value ) {
        BSONObjBuilder bob;
        value->addToBsonObj( &bob, "" );
        return bob.obj();
    }

    /** Create a Document from a BSONObj. */
    intrusive_ptr<Document> fromBson( BSONObj obj ) {
        return Document::createFromBsonObj( &obj );
    }

    namespace CoerceToBool {

        /** Nested expression coerced to true. */
        class EvaluateTrue {
        public:
            void run() {
                intrusive_ptr<Expression> nested =
                        ExpressionConstant::create( Value::createInt( 5 ) );
                intrusive_ptr<Expression> expression = ExpressionCoerceToBool::create( nested );
                ASSERT( expression->evaluate( Document::create() )->getBool() );
            }
        };

        /** Nested expression coerced to false. */
        class EvaluateFalse {
        public:
            void run() {
                intrusive_ptr<Expression> nested =
                        ExpressionConstant::create( Value::createInt( 0 ) );
                intrusive_ptr<Expression> expression = ExpressionCoerceToBool::create( nested );
                ASSERT( !expression->evaluate( Document::create() )->getBool() );
            }
        };

        /** Depdencies forwarded from nested expression. */
        class Dependencies {
        public:
            void run() {
                intrusive_ptr<Expression> nested = ExpressionFieldPath::create( "a.b" );
                intrusive_ptr<Expression> expression = ExpressionCoerceToBool::create( nested );
                set<string> dependencies;
                expression->addDependencies( dependencies );
                ASSERT_EQUALS( 1U, dependencies.size() );
                ASSERT_EQUALS( 1U, dependencies.count( "a.b" ) );
            }
        };

        // TODO Test optimize(), difficult because a CoerceToBool cannot be output as BSON.
        
    } // namespace CoerceToBool

    namespace Constant {

        /** Create an ExpressionConstant from a Value. */
        class Create {
        public:
            void run() {
                intrusive_ptr<Expression> expression =
                        ExpressionConstant::create( Value::createInt( 5 ) );
                assertBinaryEqual( BSON( "" << 5 ),
                                   toBson( expression->evaluate( Document::create() ) ) );
            }
        };

        /** Create an ExpressionConstant from a BsonElement. */
        class CreateFromBsonElement {
        public:
            void run() {
                BSONObj spec = BSON( "IGNORED_FIELD_NAME" << "foo" );
                BSONElement specElement = spec.firstElement();
                intrusive_ptr<Expression> expression =
                        ExpressionConstant::createFromBsonElement( &specElement );
                assertBinaryEqual( BSON( "" << "foo" ),
                                   toBson( expression->evaluate( Document::create() ) ) );
            }
        };

        /** No optimization is performed. */
        class Optimize {
        public:
            void run() {
                intrusive_ptr<Expression> expression =
                        ExpressionConstant::create( Value::createInt( 5 ) );
                // An attempt to optimize returns the Expression itself.
                ASSERT_EQUALS( expression, expression->optimize() );
            }
        };
        
        /** No dependencies. */
        class Dependencies {
        public:
            void run() {
                intrusive_ptr<Expression> expression =
                        ExpressionConstant::create( Value::createInt( 5 ) );
                set<string> dependencies;
                expression->addDependencies( dependencies );
                ASSERT_EQUALS( 0U, dependencies.size() );
            }
        };

        /** Output to BSONObj. */
        class AddToBsonObj {
        public:
            void run() {
                intrusive_ptr<Expression> expression =
                        ExpressionConstant::create( Value::createInt( 5 ) );
                // The constant is copied out as is.
                assertBinaryEqual( BSON( "field" << 5 ), toBsonObj( expression, false ) );
                // The constant is replaced with a $ expression.
                assertBinaryEqual( BSON( "field" << BSON( "$const" << 5 ) ),
                                   toBsonObj( expression, true ) );
            }
        private:
            static BSONObj toBsonObj( const intrusive_ptr<Expression>& expression,
                                      bool requireExpression ) {
                BSONObjBuilder bob;
                expression->addToBsonObj( &bob, "field", requireExpression );
                return bob.obj();
            }
        };

        /** Output to BSONArray. */
        class AddToBsonArray {
        public:
            void run() {
                intrusive_ptr<Expression> expression =
                        ExpressionConstant::create( Value::createInt( 5 ) );
                // The constant is copied out as is.
                assertBinaryEqual( BSON_ARRAY( 5 ), toBsonArray( expression ) );
            }
        private:
            static BSONObj toBsonArray( const intrusive_ptr<Expression>& expression ) {
                BSONArrayBuilder bab;
                expression->addToBsonArray( &bab );
                return bab.obj();
            }            
        };

    } // namespace Constant

    namespace FieldPath {

        /** The provided field path does not pass validation. */
        class Invalid {
        public:
            void run() {
                ASSERT_THROWS( ExpressionFieldPath::create( "" ), UserException );
            }
        };

        /** No optimization is performed. */
        class Optimize {
        public:
            void run() {
                intrusive_ptr<Expression> expression = ExpressionFieldPath::create( "a" );
                // An attempt to optimize returns the Expression itself.
                ASSERT_EQUALS( expression, expression->optimize() );
            }
        };

        /** The field path itself is a dependency. */
        class Dependencies {
        public:
            void run() {
                intrusive_ptr<Expression> expression = ExpressionFieldPath::create( "a.b" );
                set<string> dependencies;
                expression->addDependencies( dependencies );
                ASSERT_EQUALS( 1U, dependencies.size() );
                ASSERT_EQUALS( 1U, dependencies.count( "a.b" ) );
            }
        };

        /** Field path target field is missing. */
        class Missing {
        public:
            void run() {
                intrusive_ptr<Expression> expression = ExpressionFieldPath::create( "a" );
                // Result is undefined.
                assertBinaryEqual( fromjson( "{'':undefined}" ),
                                   toBson( expression->evaluate( Document::create() ) ) );
            }
        };

        /** Simple case where the target field is present. */
        class Present {
        public:
            void run() {
                intrusive_ptr<Expression> expression = ExpressionFieldPath::create( "a" );
                assertBinaryEqual( fromjson( "{'':123}" ),
                                   toBson( expression->evaluate
                                          ( fromBson( BSON( "a" << 123 ) ) ) ) );
            }
        };

        /** Target field parent is null. */
        class NestedBelowNull {
        public:
            void run() {
                intrusive_ptr<Expression> expression = ExpressionFieldPath::create( "a.b" );
                assertBinaryEqual( fromjson( "{'':undefined}" ),
                                   toBson( expression->evaluate
                                          ( fromBson( fromjson( "{a:null}" ) ) ) ) );
            }
        };
        
        /** Target field parent is undefined. */
        class NestedBelowUndefined {
        public:
            void run() {
                intrusive_ptr<Expression> expression = ExpressionFieldPath::create( "a.b" );
                assertBinaryEqual( fromjson( "{'':undefined}" ),
                                   toBson( expression->evaluate
                                          ( fromBson( fromjson( "{a:undefined}" ) ) ) ) );
            }
        };
        
        /** Target field parent is an integer. */
        class NestedBelowInt {
        public:
            void run() {
                intrusive_ptr<Expression> expression = ExpressionFieldPath::create( "a.b" );
                assertBinaryEqual( fromjson( "{'':undefined}" ),
                                   toBson( expression->evaluate
                                          ( fromBson( BSON( "a" << 2 ) ) ) ) );
            }
        };

        /** A value in a nested object. */
        class NestedValue {
        public:
            void run() {
                intrusive_ptr<Expression> expression = ExpressionFieldPath::create( "a.b" );
                assertBinaryEqual( BSON( "" << 55 ),
                                   toBson( expression->evaluate
                                          ( fromBson( BSON( "a" << BSON( "b" << 55 ) ) ) ) ) );
            }            
        };
        
        /** Target field within an empty object. */
        class NestedBelowEmptyObject {
        public:
            void run() {
                intrusive_ptr<Expression> expression = ExpressionFieldPath::create( "a.b" );
                assertBinaryEqual( fromjson( "{'':undefined}" ),
                                   toBson( expression->evaluate
                                          ( fromBson( BSON( "a" << BSONObj() ) ) ) ) );
            }            
        };
        
        /** Target field within an empty array. */
        class NestedBelowEmptyArray {
        public:
            void run() {
                intrusive_ptr<Expression> expression = ExpressionFieldPath::create( "a.b" );
                assertBinaryEqual( BSON( "" << BSONArray() ),
                                   toBson( expression->evaluate
                                          ( fromBson( BSON( "a" << BSONArray() ) ) ) ) );
            }            
        };
        
        /** Target field within an array containing null. */
        class NestedBelowArrayWithNull {
        public:
            void run() {
                intrusive_ptr<Expression> expression = ExpressionFieldPath::create( "a.b" );
                assertBinaryEqual( fromjson( "{'':[null]}" ),
                                   toBson( expression->evaluate
                                          ( fromBson( fromjson( "{a:[null]}" ) ) ) ) );
            }            
        };
        
        /** Target field within an array containing undefined. */
        class NestedBelowArrayWithUndefined {
        public:
            void run() {
                intrusive_ptr<Expression> expression = ExpressionFieldPath::create( "a.b" );
                assertBinaryEqual( fromjson( "{'':[undefined]}" ),
                                   toBson( expression->evaluate
                                          ( fromBson( fromjson( "{a:[undefined]}" ) ) ) ) );
            }            
        };
        
        /** Target field within an array containing an integer. */
        class NestedBelowArrayWithInt {
        public:
            void run() {
                intrusive_ptr<Expression> expression = ExpressionFieldPath::create( "a.b" );
                ASSERT_THROWS( expression->evaluate( fromBson( fromjson( "{a:[1]}" ) ) ),
                               UserException );
            }            
        };

        /** Target field within an array. */
        class NestedWithinArray {
        public:
            void run() {
                intrusive_ptr<Expression> expression = ExpressionFieldPath::create( "a.b" );
                assertBinaryEqual( fromjson( "{'':[9]}" ),
                                   toBson( expression->evaluate
                                          ( fromBson( fromjson( "{a:[{b:9}]}" ) ) ) ) );
            }
        };

        /** Multiple value types within an array. */
        class MultipleArrayValues {
        public:
            void run() {
                intrusive_ptr<Expression> expression = ExpressionFieldPath::create( "a.b" );
                assertBinaryEqual( fromjson( "{'':[9,null,undefined,undefined,20,undefined]}" ),
                                    toBson( expression->evaluate
                                           ( fromBson( fromjson
                                                      ( "{a:[{b:9},null,undefined,{g:4},{b:20},{}]}"
                                                       ) ) ) ) );
            }            
        };

        /** Expanding values within nested arrays. */
        class ExpandNestedArrays {
        public:
            void run() {
                intrusive_ptr<Expression> expression = ExpressionFieldPath::create( "a.b.c" );
                assertBinaryEqual( fromjson( "{'':[[1,2],3,[4],[[5]],[6,7]]}" ),
                                   toBson
                                    ( expression->evaluate
                                     ( fromBson
                                      ( fromjson( "{a:[{b:[{c:1},{c:2}]},"
                                                 "{b:{c:3}},"
                                                 "{b:[{c:4}]},"
                                                 "{b:[{c:[5]}]},"
                                                 "{b:{c:[6,7]}}]}" ) ) ) ) );
            }
        };

        /** Add to a BSONObj. */
        class AddToBsonObj {
        public:
            void run() {
                intrusive_ptr<Expression> expression = ExpressionFieldPath::create( "a.b.c" );
                BSONObjBuilder bob;
                expression->addToBsonObj( &bob, "foo", false );
                assertBinaryEqual( BSON( "foo" << "$a.b.c" ), bob.obj() );
            }
        };

        /** Add to a BSONArray. */
        class AddToBsonArray {
        public:
            void run() {
                intrusive_ptr<Expression> expression = ExpressionFieldPath::create( "a.b.c" );
                BSONArrayBuilder bab;
                expression->addToBsonArray( &bab );
                assertBinaryEqual( BSON_ARRAY( "$a.b.c" ), bab.arr() );
            }
        };
        
    } // namespace FieldPath

    class All : public Suite {
    public:
        All() : Suite( "expression" ) {
        }
        void setupTests() {
            add<CoerceToBool::EvaluateTrue>();
            add<CoerceToBool::EvaluateFalse>();
            add<CoerceToBool::Dependencies>();

            add<Constant::Create>();
            add<Constant::CreateFromBsonElement>();
            add<Constant::Optimize>();
            add<Constant::Dependencies>();
            add<Constant::AddToBsonObj>();
            add<Constant::AddToBsonArray>();

            add<FieldPath::Invalid>();
            add<FieldPath::Optimize>();
            add<FieldPath::Dependencies>();
            add<FieldPath::Missing>();
            add<FieldPath::Present>();
            add<FieldPath::NestedBelowNull>();
            add<FieldPath::NestedBelowUndefined>();
            add<FieldPath::NestedBelowInt>();
            add<FieldPath::NestedValue>();
            add<FieldPath::NestedBelowEmptyObject>();
            add<FieldPath::NestedBelowEmptyArray>();
            add<FieldPath::NestedBelowEmptyArray>();
            add<FieldPath::NestedBelowArrayWithNull>();
            add<FieldPath::NestedBelowArrayWithUndefined>();
            add<FieldPath::NestedBelowArrayWithInt>();
            add<FieldPath::NestedWithinArray>();
            add<FieldPath::MultipleArrayValues>();
            add<FieldPath::ExpandNestedArrays>();
            add<FieldPath::AddToBsonObj>();
            add<FieldPath::AddToBsonArray>();
        }
    } myall;

} // namespace ExpressionTests
