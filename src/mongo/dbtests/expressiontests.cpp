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

    /** Convert Expression to BSON. */
    static BSONObj expressionToBson( const intrusive_ptr<Expression>& expression ) {
        BSONObjBuilder bob;
        expression->addToBsonObj( &bob, "", true );
        return bob.obj().firstElement().embeddedObject().getOwned();
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

        /** Output to BSONObj. */
        class AddToBsonObj {
        public:
            void run() {
                intrusive_ptr<Expression> expression = ExpressionCoerceToBool::create(
                        ExpressionFieldPath::create("foo"));

                // serialized as $and because CoerceToBool isn't an ExpressionNary
                assertBinaryEqual(fromjson("{field:{$and:['$foo']}}"), toBsonObj(expression));
            }
        private:
            static BSONObj toBsonObj(const intrusive_ptr<Expression>& expression) {

                BSONObjBuilder bob;
                expression->addToBsonObj(&bob, "field", false);
                return bob.obj();
            }
        };

        /** Output to BSONArray. */
        class AddToBsonArray {
        public:
            void run() {
                intrusive_ptr<Expression> expression = ExpressionCoerceToBool::create(
                        ExpressionFieldPath::create("foo"));

                // serialized as $and because CoerceToBool isn't an ExpressionNary
                assertBinaryEqual(BSON_ARRAY(fromjson("{$and:['$foo']}")), toBsonArray(expression));
            }
        private:
            static BSONArray toBsonArray(const intrusive_ptr<Expression>& expression) {
                BSONArrayBuilder bab;
                expression->addToBsonArray(&bab);
                return bab.arr();
            }
        };


        // TODO Test optimize(), difficult because a CoerceToBool cannot be output as BSON.
        
    } // namespace CoerceToBool

    namespace Compare {

        class OptimizeBase {
        public:
            virtual ~OptimizeBase() {
            }
            void run() {
                BSONObj specObject = BSON( "" << spec() );
                BSONElement specElement = specObject.firstElement();
                intrusive_ptr<Expression> expression = Expression::parseOperand( &specElement );
                intrusive_ptr<Expression> optimized = expression->optimize();
                ASSERT_EQUALS( expectedFieldRange(),
                               (bool)dynamic_pointer_cast<ExpressionFieldRange>( optimized ) );
                ASSERT_EQUALS( expectedOptimized(), expressionToBson( optimized ) );
            }
        protected:
            virtual BSONObj spec() = 0;
            virtual BSONObj expectedOptimized() = 0;
            virtual bool expectedFieldRange() = 0;
        };
        
        class FieldRangeOptimize : public OptimizeBase {
            BSONObj expectedOptimized() { return spec(); }
            bool expectedFieldRange() { return true; }
        };
        
        class NoOptimize : public OptimizeBase {
            BSONObj expectedOptimized() { return spec(); }
            bool expectedFieldRange() { return false; }
        };

        /** Check expected result for expressions depending on constants. */
        class ExpectedResultBase : public OptimizeBase {
        public:
            void run() {
                OptimizeBase::run();
                BSONObj specObject = BSON( "" << spec() );
                BSONElement specElement = specObject.firstElement();
                intrusive_ptr<Expression> expression = Expression::parseOperand( &specElement );
                // Check expression spec round trip.
                ASSERT_EQUALS( spec(), expressionToBson( expression ) );
                // Check evaluation result.
                ASSERT_EQUALS( expectedResult(),
                               toBson( expression->evaluate( Document::create() ) ) );
                // Check that the result is the same after optimizing.
                intrusive_ptr<Expression> optimized = expression->optimize();
                ASSERT_EQUALS( expectedResult(),
                               toBson( optimized->evaluate( Document::create() ) ) );
            }
        protected:
            virtual BSONObj spec() = 0;
            virtual BSONObj expectedResult() = 0;
        private:
            virtual BSONObj expectedOptimized() {
                return BSON( "$const" << expectedResult().firstElement() );
            }
            virtual bool expectedFieldRange() { return false; }
        };

        class ExpectedTrue : public ExpectedResultBase {
            BSONObj expectedResult() { return BSON( "" << true ); }            
        };
        
        class ExpectedFalse : public ExpectedResultBase {
            BSONObj expectedResult() { return BSON( "" << false ); }            
        };
        
        class ParseError {
        public:
            virtual ~ParseError() {
            }
            void run() {
                BSONObj specObject = BSON( "" << spec() );
                BSONElement specElement = specObject.firstElement();
                ASSERT_THROWS( Expression::parseOperand( &specElement ), UserException );
            }
        protected:
            virtual BSONObj spec() = 0;
        };

        /** $eq with first < second. */
        class EqLt : public ExpectedFalse {
            BSONObj spec() { return BSON( "$eq" << BSON_ARRAY( 1 << 2 ) ); }
        };

        /** $eq with first == second. */
        class EqEq : public ExpectedTrue {
            BSONObj spec() { return BSON( "$eq" << BSON_ARRAY( 1 << 1 ) ); }
        };
        
        /** $eq with first > second. */
        class EqGt : public ExpectedFalse {
            BSONObj spec() { return BSON( "$eq" << BSON_ARRAY( 1 << 0 ) ); }
        };
        
        /** $ne with first < second. */
        class NeLt : public ExpectedTrue {
            BSONObj spec() { return BSON( "$ne" << BSON_ARRAY( 1 << 2 ) ); }
        };
        
        /** $ne with first == second. */
        class NeEq : public ExpectedFalse {
            BSONObj spec() { return BSON( "$ne" << BSON_ARRAY( 1 << 1 ) ); }
        };
        
        /** $ne with first > second. */
        class NeGt : public ExpectedTrue {
            BSONObj spec() { return BSON( "$ne" << BSON_ARRAY( 1 << 0 ) ); }
        };
        
        /** $gt with first < second. */
        class GtLt : public ExpectedFalse {
            BSONObj spec() { return BSON( "$gt" << BSON_ARRAY( 1 << 2 ) ); }
        };
        
        /** $gt with first == second. */
        class GtEq : public ExpectedFalse {
            BSONObj spec() { return BSON( "$gt" << BSON_ARRAY( 1 << 1 ) ); }
        };
        
        /** $gt with first > second. */
        class GtGt : public ExpectedTrue {
            BSONObj spec() { return BSON( "$gt" << BSON_ARRAY( 1 << 0 ) ); }
        };
        
        /** $gte with first < second. */
        class GteLt : public ExpectedFalse {
            BSONObj spec() { return BSON( "$gte" << BSON_ARRAY( 1 << 2 ) ); }
        };
        
        /** $gte with first == second. */
        class GteEq : public ExpectedTrue {
            BSONObj spec() { return BSON( "$gte" << BSON_ARRAY( 1 << 1 ) ); }
        };
        
        /** $gte with first > second. */
        class GteGt : public ExpectedTrue {
            BSONObj spec() { return BSON( "$gte" << BSON_ARRAY( 1 << 0 ) ); }
        };
        
        /** $lt with first < second. */
        class LtLt : public ExpectedTrue {
            BSONObj spec() { return BSON( "$lt" << BSON_ARRAY( 1 << 2 ) ); }
        };
        
        /** $lt with first == second. */
        class LtEq : public ExpectedFalse {
            BSONObj spec() { return BSON( "$lt" << BSON_ARRAY( 1 << 1 ) ); }
        };
        
        /** $lt with first > second. */
        class LtGt : public ExpectedFalse {
            BSONObj spec() { return BSON( "$lt" << BSON_ARRAY( 1 << 0 ) ); }
        };
        
        /** $lte with first < second. */
        class LteLt : public ExpectedTrue {
            BSONObj spec() { return BSON( "$lte" << BSON_ARRAY( 1 << 2 ) ); }
        };
        
        /** $lte with first == second. */
        class LteEq : public ExpectedTrue {
            BSONObj spec() { return BSON( "$lte" << BSON_ARRAY( 1 << 1 ) ); }
        };
        
        /** $lte with first > second. */
        class LteGt : public ExpectedFalse {
            BSONObj spec() { return BSON( "$lte" << BSON_ARRAY( 1 << 0 ) ); }
        };
        
        /** $cmp with first < second. */
        class CmpLt : public ExpectedResultBase {
            BSONObj spec() { return BSON( "$cmp" << BSON_ARRAY( 1 << 2 ) ); }
            BSONObj expectedResult() { return BSON( "" << -1 ); }
        };
        
        /** $cmp with first == second. */
        class CmpEq : public ExpectedResultBase {
            BSONObj spec() { return BSON( "$cmp" << BSON_ARRAY( 1 << 1 ) ); }
            BSONObj expectedResult() { return BSON( "" << 0 ); }
        };
        
        /** $cmp with first > second. */
        class CmpGt : public ExpectedResultBase {
            BSONObj spec() { return BSON( "$cmp" << BSON_ARRAY( 1 << 0 ) ); }
            BSONObj expectedResult() { return BSON( "" << 1 ); }
        };

        /** $cmp results are bracketed to an absolute value of 1. */
        class CmpBracketed : public ExpectedResultBase {
            BSONObj spec() { return BSON( "$cmp" << BSON_ARRAY( "z" << "a" ) ); }
            BSONObj expectedResult() { return BSON( "" << 1 ); }
        };

        /** Zero operands provided. */
        class ZeroOperands : public ParseError {
            BSONObj spec() { return BSON( "$ne" << BSONArray() ); }
        };

        /** One operand provided. */
        class OneOperand : public ParseError {
            BSONObj spec() { return BSON( "$eq" << BSON_ARRAY( 1 ) ); }
        };
        
        /** Three operands provided. */
        class ThreeOperands : public ParseError {
            BSONObj spec() { return BSON( "$gt" << BSON_ARRAY( 2 << 3 << 4 ) ); }
        };
        
        /** Incompatible types cannot be compared. */
        class IncompatibleTypes {
        public:
            void run() {
                BSONObj specObject = BSON( "" << BSON( "$ne" << BSON_ARRAY( "a" << 1 ) ) );
                BSONElement specElement = specObject.firstElement();
                intrusive_ptr<Expression> expression = Expression::parseOperand( &specElement );
                ASSERT_THROWS( expression->evaluate( Document::create() ), UserException );
            }
        };

        /**
         * An expression depending on constants is optimized to a constant via
         * ExpressionNary::optimize().
         */
        class OptimizeConstants : public OptimizeBase {
            BSONObj spec() { return BSON( "$eq" << BSON_ARRAY( 1 << 1 ) ); }
            BSONObj expectedOptimized() { return BSON( "$const" << true ); }
            bool expectedFieldRange() { return false; }
        };

        /** $cmp is not optimized. */
        class NoOptimizeCmp : public NoOptimize {
            BSONObj spec() { return BSON( "$cmp" << BSON_ARRAY( 1 << "$a" ) ); }
        };
        
        /** $ne is not optimized. */
        class NoOptimizeNe : public NoOptimize {
            BSONObj spec() { return BSON( "$ne" << BSON_ARRAY( 1 << "$a" ) ); }
        };
        
        /** No optimization is performend without a constant. */
        class NoOptimizeNoConstant : public NoOptimize {
            BSONObj spec() { return BSON( "$ne" << BSON_ARRAY( "$a" << "$b" ) ); }
        };
        
        /** No optimization is performend without an immediate field path. */
        class NoOptimizeWithoutFieldPath : public NoOptimize {
            BSONObj spec() {
                return BSON( "$eq" << BSON_ARRAY( BSON( "$and" << BSON_ARRAY( "$a" ) ) << 1 ) );
            }
        };
        
        /** No optimization is performend without an immediate field path. */
        class NoOptimizeWithoutFieldPathReverse : public NoOptimize {
            BSONObj spec() {
                return BSON( "$eq" << BSON_ARRAY( 1 << BSON( "$and" << BSON_ARRAY( "$a" ) ) ) );
            }
        };
        
        /** An equality expression is optimized. */
        class OptimizeEq : public FieldRangeOptimize {
            BSONObj spec() { return BSON( "$eq" << BSON_ARRAY( "$a" << 1 ) ); }
        };
        
        /** A reverse sense equality expression is optimized. */
        class OptimizeEqReverse : public FieldRangeOptimize {
            BSONObj spec() { return BSON( "$eq" << BSON_ARRAY( 1 << "$a" ) ); }
            BSONObj expectedOptimized() { return BSON( "$eq" << BSON_ARRAY( "$a" << 1 ) ); }
        };
        
        /** A $lt expression is optimized. */
        class OptimizeLt : public FieldRangeOptimize {
            BSONObj spec() { return BSON( "$lt" << BSON_ARRAY( "$a" << 1 ) ); }
        };
        
        /** A reverse sense $lt expression is optimized. */
        class OptimizeLtReverse : public FieldRangeOptimize {
            BSONObj spec() { return BSON( "$lt" << BSON_ARRAY( 1 << "$a" ) ); }
            BSONObj expectedOptimized() { return BSON( "$gt" << BSON_ARRAY( "$a" << 1 ) ); }
        };
        
        /** A $lte expression is optimized. */
        class OptimizeLte : public FieldRangeOptimize {
            BSONObj spec() { return BSON( "$lte" << BSON_ARRAY( "$b" << 2 ) ); }
        };
        
        /** A reverse sense $lte expression is optimized. */
        class OptimizeLteReverse : public FieldRangeOptimize {
            BSONObj spec() { return BSON( "$lte" << BSON_ARRAY( 2 << "$b" ) ); }
            BSONObj expectedOptimized() { return BSON( "$gte" << BSON_ARRAY( "$b" << 2 ) ); }
        };
        
        /** A $gt expression is optimized. */
        class OptimizeGt : public FieldRangeOptimize {
            BSONObj spec() { return BSON( "$gt" << BSON_ARRAY( "$b" << 2 ) ); }
        };
        
        /** A reverse sense $gt expression is optimized. */
        class OptimizeGtReverse : public FieldRangeOptimize {
            BSONObj spec() { return BSON( "$gt" << BSON_ARRAY( 2 << "$b" ) ); }
            BSONObj expectedOptimized() { return BSON( "$lt" << BSON_ARRAY( "$b" << 2 ) ); }
        };
        
        /** A $gte expression is optimized. */
        class OptimizeGte : public FieldRangeOptimize {
            BSONObj spec() { return BSON( "$gte" << BSON_ARRAY( "$b" << 2 ) ); }
        };
        
        /** A reverse sense $gte expression is optimized. */
        class OptimizeGteReverse : public FieldRangeOptimize {
            BSONObj spec() { return BSON( "$gte" << BSON_ARRAY( 2 << "$b" ) ); }
            BSONObj expectedOptimized() { return BSON( "$lte" << BSON_ARRAY( "$b" << 2 ) ); }
        };
        
    } // namespace Compare
    
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
            add<CoerceToBool::AddToBsonObj>();
            add<CoerceToBool::AddToBsonArray>();

            add<Compare::EqLt>();
            add<Compare::EqEq>();
            add<Compare::EqGt>();
            add<Compare::NeLt>();
            add<Compare::NeEq>();
            add<Compare::NeGt>();
            add<Compare::GtLt>();
            add<Compare::GtEq>();
            add<Compare::GtGt>();
            add<Compare::GteLt>();
            add<Compare::GteEq>();
            add<Compare::GteGt>();
            add<Compare::LtLt>();
            add<Compare::LtEq>();
            add<Compare::LtGt>();
            add<Compare::LteLt>();
            add<Compare::LteEq>();
            add<Compare::LteGt>();
            add<Compare::CmpLt>();
            add<Compare::CmpEq>();
            add<Compare::CmpGt>();
            add<Compare::CmpBracketed>();
            add<Compare::ZeroOperands>();
            add<Compare::OneOperand>();
            add<Compare::ThreeOperands>();
            add<Compare::IncompatibleTypes>();
            add<Compare::OptimizeConstants>();
            add<Compare::NoOptimizeCmp>();
            add<Compare::NoOptimizeNe>();
            add<Compare::NoOptimizeNoConstant>();
            add<Compare::NoOptimizeWithoutFieldPath>();
            add<Compare::NoOptimizeWithoutFieldPathReverse>();
            add<Compare::OptimizeEq>();
            add<Compare::OptimizeEqReverse>();
            add<Compare::OptimizeLt>();
            add<Compare::OptimizeLtReverse>();
            add<Compare::OptimizeLte>();
            add<Compare::OptimizeLteReverse>();
            add<Compare::OptimizeGt>();
            add<Compare::OptimizeGtReverse>();
            add<Compare::OptimizeGte>();
            add<Compare::OptimizeGteReverse>();

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
