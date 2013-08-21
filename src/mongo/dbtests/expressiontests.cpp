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

#include "mongo/pch.h"

#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/dbtests/dbtests.h"

namespace ExpressionTests {

    /** Convert BSONObj to a BSONObj with our $const wrappings. */
    static BSONObj constify(const BSONObj& obj, bool parentIsArray=false) {
        BSONObjBuilder bob;
        for (BSONObjIterator itr(obj); itr.more(); itr.next()) {
            BSONElement elem = *itr;
            if (elem.type() == Object) {
                bob << elem.fieldName() << constify(elem.Obj(), false);
            }
            else if (elem.type() == Array && !parentIsArray) {
                // arrays within arrays are treated as constant values by the real parser
                bob << elem.fieldName() << BSONArray(constify(elem.Obj(), true));
            }
            else if (str::equals(elem.fieldName(), "$const") ||
                     (elem.type() == mongo::String && elem.valuestrsafe()[0] == '$')) {
                bob.append(elem);
            }
            else {
                bob.append(elem.fieldName(), BSON("$const" << elem));
            }
        }
        return bob.obj();
    }

    /** Check binary equality, ensuring use of the same numeric types. */
    static void assertBinaryEqual( const BSONObj& expected, const BSONObj& actual ) {
        ASSERT_EQUALS( expected, actual );
        ASSERT( expected.binaryEqual( actual ) );
    }

    /** Convert Value to a wrapped BSONObj with an empty string field name. */
    static BSONObj toBson( const Value& value ) {
        BSONObjBuilder bob;
        value.addToBsonObj( &bob, "" );
        return bob.obj();
    }

    /** Convert Expression to BSON. */
    static BSONObj expressionToBson( const intrusive_ptr<Expression>& expression ) {
        return BSON("" << expression->serialize()).firstElement().embeddedObject().getOwned();
    }

    /** Convert Document to BSON. */
    static BSONObj toBson( const Document& document ) {
        return document.toBson();
    }
    
    /** Create a Document from a BSONObj. */
    Document fromBson( BSONObj obj ) {
        return Document(obj);
    }

    /** Create a Value from a BSONObj. */
    Value valueFromBson( BSONObj obj ) {
        BSONElement element = obj.firstElement();
        return Value( element );
    }
    
    namespace Add {

        class ExpectedResultBase {
        public:
            virtual ~ExpectedResultBase() {}
            void run() {
                intrusive_ptr<ExpressionNary> expression = new ExpressionAdd();
                populateOperands( expression );
                ASSERT_EQUALS( expectedResult(),
                               toBson( expression->evaluate( Document() ) ) );
            }
        protected:
            virtual void populateOperands( intrusive_ptr<ExpressionNary>& expression ) = 0;
            virtual BSONObj expectedResult() = 0;
        };

        /** $add with a NULL Document pointer, as called by ExpressionNary::optimize(). */
        class NullDocument {
        public:
            void run() {
                intrusive_ptr<ExpressionNary> expression = new ExpressionAdd();
                expression->addOperand( ExpressionConstant::create( Value( 2 ) ) );
                ASSERT_EQUALS( BSON( "" << 2 ), toBson( expression->evaluate( Document() ) ) );
            }
        };

        /** $add without operands. */
        class NoOperands : public ExpectedResultBase {
            void populateOperands( intrusive_ptr<ExpressionNary>& expression ) {}
            virtual BSONObj expectedResult() { return BSON( "" << 0 ); }
        };

        /** String type unsupported. */
        class String {
        public:
            void run() {
                intrusive_ptr<ExpressionNary> expression = new ExpressionAdd();
                expression->addOperand( ExpressionConstant::create( Value( "a" ) ) );
                ASSERT_THROWS( expression->evaluate( Document() ), UserException );
            }
        };

        /** Bool type unsupported. */
        class Bool {
        public:
            void run() {
                intrusive_ptr<ExpressionNary> expression = new ExpressionAdd();
                expression->addOperand( ExpressionConstant::create( Value(true) ) );
                ASSERT_THROWS( expression->evaluate( Document() ), UserException );
            }            
        };

        class SingleOperandBase : public ExpectedResultBase {
            void populateOperands( intrusive_ptr<ExpressionNary>& expression ) {
                expression->addOperand( ExpressionConstant::create( valueFromBson( operand() ) ) );
            }
            BSONObj expectedResult() { return operand(); }
        protected:
            virtual BSONObj operand() = 0;
        };

        /** Single int argument. */
        class Int : public SingleOperandBase {
            BSONObj operand() { return BSON( "" << 1 ); }
        };
        
        /** Single long argument. */
        class Long : public SingleOperandBase {
            BSONObj operand() { return BSON( "" << 5555LL ); }
        };
        
        /** Single double argument. */
        class Double : public SingleOperandBase {
            BSONObj operand() { return BSON( "" << 99.99 ); }
        };
        
        /** Single Date argument. */
        class Date : public SingleOperandBase {
            BSONObj operand() { return BSON( "" << Date_t(12345) ); }
        };

        /** Single null argument. */
        class Null : public SingleOperandBase {
            BSONObj operand() { return BSON( "" << BSONNULL ); }
            BSONObj expectedResult() { return BSON( "" << BSONNULL ); }
        };
        
        /** Single undefined argument. */
        class Undefined : public SingleOperandBase {
            BSONObj operand() { return fromjson( "{'':undefined}" ); }
            BSONObj expectedResult() { return BSON( "" << BSONNULL ); }
        };
        
        class TwoOperandBase : public ExpectedResultBase {
        public:
            TwoOperandBase() :
                _reverse() {
            }
            void run() {
                ExpectedResultBase::run();
                // Now add the operands in the reverse direction.
                _reverse = true;
                ExpectedResultBase::run();
            }
        protected:
            void populateOperands( intrusive_ptr<ExpressionNary>& expression ) {
                expression->addOperand( ExpressionConstant::create
                                        ( valueFromBson( _reverse ? operand2() : operand1() ) ) );
                expression->addOperand( ExpressionConstant::create
                                        ( valueFromBson( _reverse ? operand1() : operand2() ) ) );
            }
            virtual BSONObj operand1() = 0;
            virtual BSONObj operand2() = 0;
        private:
            bool _reverse;
        };

        /** Add two ints. */
        class IntInt : public TwoOperandBase {
            BSONObj operand1() { return BSON( "" << 1 ); }
            BSONObj operand2() { return BSON( "" << 5 ); }
            BSONObj expectedResult() { return BSON( "" << 6 ); }
        };

        /** Adding two large ints produces a long, not an overflowed int. */
        class IntIntNoOverflow : public TwoOperandBase {
            BSONObj operand1() { return BSON( "" << numeric_limits<int>::max() ); }
            BSONObj operand2() { return BSON( "" << numeric_limits<int>::max() ); }
            BSONObj expectedResult() {
                return BSON( "" << ( (long long)( numeric_limits<int>::max() ) +
                                     numeric_limits<int>::max() ) );
            }
        };

        /** Adding an int and a long produces a long. */
        class IntLong : public TwoOperandBase {
            BSONObj operand1() { return BSON( "" << 1 ); }
            BSONObj operand2() { return BSON( "" << 9LL ); }
            BSONObj expectedResult() { return BSON( "" << 10LL ); }
        };

        /** Adding an int and a long overflows. */
        class IntLongOverflow : public TwoOperandBase {
            BSONObj operand1() { return BSON( "" << numeric_limits<int>::max() ); }
            BSONObj operand2() { return BSON( "" << numeric_limits<long long>::max() ); }
            BSONObj expectedResult() { return BSON( "" << ( numeric_limits<int>::max()
                                                           + numeric_limits<long long>::max() ) ); }
        };

        /** Adding an int and a double produces a double. */
        class IntDouble : public TwoOperandBase {
            BSONObj operand1() { return BSON( "" << 9 ); }
            BSONObj operand2() { return BSON( "" << 1.1 ); }
            BSONObj expectedResult() { return BSON( "" << 10.1 ); }
        };

        /** Adding an int and a Date produces a Date. */
        class IntDate : public TwoOperandBase {
            BSONObj operand1() { return BSON( "" << 6 ); }
            BSONObj operand2() { return BSON( "" << Date_t(123450) ); }
            BSONObj expectedResult() { return BSON( "" << Date_t(123456) ); }
        };
        
        /** Adding a long and a double produces a double. */
        class LongDouble : public TwoOperandBase {
            BSONObj operand1() { return BSON( "" << 9LL ); }
            BSONObj operand2() { return BSON( "" << 1.1 ); }
            BSONObj expectedResult() { return BSON( "" << 10.1 ); }
        };
        
        /** Adding a long and a double does not overflow. */
        class LongDoubleNoOverflow : public TwoOperandBase {
            BSONObj operand1() { return BSON( "" << numeric_limits<long long>::max() ); }
            BSONObj operand2() { return BSON( "" << double( numeric_limits<long long>::max() ) ); }
            BSONObj expectedResult() {
                return BSON( "" << numeric_limits<long long>::max()
                             + double( numeric_limits<long long>::max() ) );
            }
        };
        
        /** Adding an int and null. */
        class IntNull : public TwoOperandBase {
            BSONObj operand1() { return BSON( "" << 1 ); }
            BSONObj operand2() { return BSON( "" << BSONNULL ); }
            BSONObj expectedResult() { return BSON( "" << BSONNULL ); }
        };
        
        /** Adding a long and undefined. */
        class LongUndefined : public TwoOperandBase {
            BSONObj operand1() { return BSON( "" << 5LL ); }
            BSONObj operand2() { return fromjson( "{'':undefined}" ); }
            BSONObj expectedResult() { return BSON( "" << BSONNULL ); }
        };
        
    } // namespace Add

    namespace And {

        class ExpectedResultBase {
        public:
            virtual ~ExpectedResultBase() {
            }
            void run() {
                BSONObj specObject = BSON( "" << spec() );
                BSONElement specElement = specObject.firstElement();
                intrusive_ptr<Expression> expression = Expression::parseOperand( specElement );
                ASSERT_EQUALS( constify( spec() ), expressionToBson( expression ) );
                ASSERT_EQUALS( BSON( "" << expectedResult() ),
                               toBson( expression->evaluate( fromBson( BSON( "a" << 1 ) ) ) ) );
                intrusive_ptr<Expression> optimized = expression->optimize();
                ASSERT_EQUALS( BSON( "" << expectedResult() ),
                               toBson( optimized->evaluate( fromBson( BSON( "a" << 1 ) ) ) ) );
            }
        protected:
            virtual BSONObj spec() = 0;
            virtual bool expectedResult() = 0;
        };

        class OptimizeBase {
        public:
            virtual ~OptimizeBase() {
            }
            void run() {
                BSONObj specObject = BSON( "" << spec() );
                BSONElement specElement = specObject.firstElement();
                intrusive_ptr<Expression> expression = Expression::parseOperand( specElement );
                ASSERT_EQUALS( constify( spec() ), expressionToBson( expression ) );
                intrusive_ptr<Expression> optimized = expression->optimize();
                ASSERT_EQUALS( expectedOptimized(), expressionToBson( optimized ) );
            }
        protected:
            virtual BSONObj spec() = 0;
            virtual BSONObj expectedOptimized() = 0;
        };

        class NoOptimizeBase : public OptimizeBase {
            BSONObj expectedOptimized() { return constify( spec() ); }
        };

        /** $and without operands. */
        class NoOperands : public ExpectedResultBase {
            BSONObj spec() { return BSON( "$and" << BSONArray() ); }
            bool expectedResult() { return true; }
        };

        /** $and passed 'true'. */
        class True : public ExpectedResultBase {
            BSONObj spec() { return BSON( "$and" << BSON_ARRAY( true ) ); }
            bool expectedResult() { return true; }
        };
        
        /** $and passed 'false'. */
        class False : public ExpectedResultBase {
            BSONObj spec() { return BSON( "$and" << BSON_ARRAY( false ) ); }
            bool expectedResult() { return false; }
        };
        
        /** $and passed 'true', 'true'. */
        class TrueTrue : public ExpectedResultBase {
            BSONObj spec() { return BSON( "$and" << BSON_ARRAY( true << true ) ); }
            bool expectedResult() { return true; }
        };
        
        /** $and passed 'true', 'false'. */
        class TrueFalse : public ExpectedResultBase {
            BSONObj spec() { return BSON( "$and" << BSON_ARRAY( true << false ) ); }
            bool expectedResult() { return false; }
        };
        
        /** $and passed 'false', 'true'. */
        class FalseTrue : public ExpectedResultBase {
            BSONObj spec() { return BSON( "$and" << BSON_ARRAY( false << true ) ); }
            bool expectedResult() { return false; }
        };
        
        /** $and passed 'false', 'false'. */
        class FalseFalse : public ExpectedResultBase {
            BSONObj spec() { return BSON( "$and" << BSON_ARRAY( false << false ) ); }
            bool expectedResult() { return false; }
        };
        
        /** $and passed 'true', 'true', 'true'. */
        class TrueTrueTrue : public ExpectedResultBase {
            BSONObj spec() { return BSON( "$and" << BSON_ARRAY( true << true << true ) ); }
            bool expectedResult() { return true; }
        };

        /** $and passed 'true', 'true', 'false'. */
        class TrueTrueFalse : public ExpectedResultBase {
            BSONObj spec() { return BSON( "$and" << BSON_ARRAY( true << true << false ) ); }
            bool expectedResult() { return false; }
        };

        /** $and passed '0', '1'. */
        class ZeroOne : public ExpectedResultBase {
            BSONObj spec() { return BSON( "$and" << BSON_ARRAY( 0 << 1 ) ); }
            bool expectedResult() { return false; }
        };
        
        /** $and passed '1', '2'. */
        class OneTwo : public ExpectedResultBase {
            BSONObj spec() { return BSON( "$and" << BSON_ARRAY( 1 << 2 ) ); }
            bool expectedResult() { return true; }
        };
        
        /** $and passed a field path. */
        class FieldPath : public ExpectedResultBase {
            BSONObj spec() { return BSON( "$and" << BSON_ARRAY( "$a" ) ); }
            bool expectedResult() { return true; }
        };

        /** A constant expression is optimized to a constant. */
        class OptimizeConstantExpression : public OptimizeBase {
            BSONObj spec() { return BSON( "$and" << BSON_ARRAY( 1 ) ); }
            BSONObj expectedOptimized() { return BSON( "$const" << true ); }
        };

        /** A non constant expression is not optimized. */
        class NonConstant : public NoOptimizeBase {
            BSONObj spec() { return BSON( "$and" << BSON_ARRAY( "$a" ) ); }            
        };

        /** An expression beginning with a single constant is optimized. */
        class ConstantNonConstantTrue : public OptimizeBase {
            BSONObj spec() { return BSON( "$and" << BSON_ARRAY( 1 << "$a" ) ); }
            BSONObj expectedOptimized() { return BSON( "$and" << BSON_ARRAY( "$a" ) ); }
            // note: using $and as serialization of ExpressionCoerceToBool rather than ExpressionAnd
        };

        class ConstantNonConstantFalse : public OptimizeBase {
            BSONObj spec() { return BSON( "$and" << BSON_ARRAY( 0 << "$a" ) ); }
            BSONObj expectedOptimized() { return BSON( "$const" << false ); }
        };

        /** An expression with a field path and '1'. */
        class NonConstantOne : public OptimizeBase {
            BSONObj spec() { return BSON( "$and" << BSON_ARRAY( "$a" << 1 ) ); }
            BSONObj expectedOptimized() { return BSON( "$and" << BSON_ARRAY( "$a" ) ); }
        };
        
        /** An expression with a field path and '0'. */
        class NonConstantZero : public OptimizeBase {
            BSONObj spec() { return BSON( "$and" << BSON_ARRAY( "$a" << 0 ) ); }
            BSONObj expectedOptimized() { return BSON( "$const" << false ); }
        };
        
        /** An expression with two field paths and '1'. */
        class NonConstantNonConstantOne : public OptimizeBase {
            BSONObj spec() { return BSON( "$and" << BSON_ARRAY( "$a" << "$b" << 1 ) ); }
            BSONObj expectedOptimized() { return BSON( "$and" << BSON_ARRAY( "$a" << "$b" ) ); }
        };
        
        /** An expression with two field paths and '0'. */
        class NonConstantNonConstantZero : public OptimizeBase {
            BSONObj spec() { return BSON( "$and" << BSON_ARRAY( "$a" << "$b" << 0 ) ); }
            BSONObj expectedOptimized() { return BSON( "$const" << false ); }
        };

        /** An expression with '0', '1', and a field path. */
        class ZeroOneNonConstant : public OptimizeBase {
            BSONObj spec() { return BSON( "$and" << BSON_ARRAY( 0 << 1 << "$a" ) ); }
            BSONObj expectedOptimized() { return BSON( "$const" << false ); }
        };
        
        /** An expression with '1', '1', and a field path. */
        class OneOneNonConstant : public OptimizeBase {
            BSONObj spec() { return BSON( "$and" << BSON_ARRAY( 1 << 1 << "$a" ) ); }
            BSONObj expectedOptimized() { return BSON( "$and" << BSON_ARRAY( "$a" ) ); }            
        };

        /** Nested $and expressions. */
        class Nested : public OptimizeBase {
            BSONObj spec() {
                return BSON( "$and" <<
                             BSON_ARRAY( 1 << BSON( "$and" << BSON_ARRAY( 1 ) ) << "$a" << "$b" ) );
            }
            BSONObj expectedOptimized() { return BSON( "$and" << BSON_ARRAY( "$a" << "$b" ) ); }            
        };

        /** Nested $and expressions containing a nested value evaluating to false. */
        class NestedZero : public OptimizeBase {
            BSONObj spec() {
                return BSON( "$and" <<
                            BSON_ARRAY( 1 <<
                                        BSON( "$and" <<
                                              BSON_ARRAY( BSON( "$and" <<
                                                                BSON_ARRAY( 0 ) ) ) ) <<
                                        "$a" << "$b" ) );
            }
            BSONObj expectedOptimized() { return BSON( "$const" << false ); }
        };
        
    } // namespace And

    namespace CoerceToBool {

        /** Nested expression coerced to true. */
        class EvaluateTrue {
        public:
            void run() {
                intrusive_ptr<Expression> nested =
                        ExpressionConstant::create( Value( 5 ) );
                intrusive_ptr<Expression> expression = ExpressionCoerceToBool::create( nested );
                ASSERT( expression->evaluate( Document() ).getBool() );
            }
        };

        /** Nested expression coerced to false. */
        class EvaluateFalse {
        public:
            void run() {
                intrusive_ptr<Expression> nested =
                        ExpressionConstant::create( Value( 0 ) );
                intrusive_ptr<Expression> expression = ExpressionCoerceToBool::create( nested );
                ASSERT( !expression->evaluate( Document() ).getBool() );
            }
        };

        /** Dependencies forwarded from nested expression. */
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
                return BSON("field" << expression->serialize());
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
                bab << expression->serialize();
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
                intrusive_ptr<Expression> expression = Expression::parseOperand( specElement );
                intrusive_ptr<Expression> optimized = expression->optimize();
                ASSERT_EQUALS( expectedFieldRange(),
                               (bool)dynamic_pointer_cast<ExpressionFieldRange>( optimized ) );
                ASSERT_EQUALS( constify( expectedOptimized() ), expressionToBson( optimized ) );
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
                intrusive_ptr<Expression> expression = Expression::parseOperand( specElement );
                // Check expression spec round trip.
                ASSERT_EQUALS( constify( spec() ), expressionToBson( expression ) );
                // Check evaluation result.
                ASSERT_EQUALS( expectedResult(),
                               toBson( expression->evaluate( Document() ) ) );
                // Check that the result is the same after optimizing.
                intrusive_ptr<Expression> optimized = expression->optimize();
                ASSERT_EQUALS( expectedResult(),
                               toBson( optimized->evaluate( Document() ) ) );
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
                ASSERT_THROWS( Expression::parseOperand( specElement ), UserException );
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
        
        /** Incompatible types can be compared. */
        class IncompatibleTypes {
        public:
            void run() {
                BSONObj specObject = BSON( "" << BSON( "$ne" << BSON_ARRAY( "a" << 1 ) ) );
                BSONElement specElement = specObject.firstElement();
                intrusive_ptr<Expression> expression = Expression::parseOperand( specElement );
                ASSERT_EQUALS(expression->evaluate(Document()), Value(true));
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
                        ExpressionConstant::create( Value( 5 ) );
                assertBinaryEqual( BSON( "" << 5 ),
                                   toBson( expression->evaluate( Document() ) ) );
            }
        };

        /** Create an ExpressionConstant from a BsonElement. */
        class CreateFromBsonElement {
        public:
            void run() {
                BSONObj spec = BSON( "IGNORED_FIELD_NAME" << "foo" );
                BSONElement specElement = spec.firstElement();
                intrusive_ptr<Expression> expression =
                        ExpressionConstant::parse( specElement );
                assertBinaryEqual( BSON( "" << "foo" ),
                                   toBson( expression->evaluate( Document() ) ) );
            }
        };

        /** No optimization is performed. */
        class Optimize {
        public:
            void run() {
                intrusive_ptr<Expression> expression =
                        ExpressionConstant::create( Value( 5 ) );
                // An attempt to optimize returns the Expression itself.
                ASSERT_EQUALS( expression, expression->optimize() );
            }
        };
        
        /** No dependencies. */
        class Dependencies {
        public:
            void run() {
                intrusive_ptr<Expression> expression =
                        ExpressionConstant::create( Value( 5 ) );
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
                        ExpressionConstant::create( Value( 5 ) );
                // The constant is replaced with a $ expression.
                assertBinaryEqual( BSON( "field" << BSON( "$const" << 5 ) ),
                                   toBsonObj( expression ) );
            }
        private:
            static BSONObj toBsonObj( const intrusive_ptr<Expression>& expression ) {
                return BSON("field" << expression->serialize());
            }
        };

        /** Output to BSONArray. */
        class AddToBsonArray {
        public:
            void run() {
                intrusive_ptr<Expression> expression =
                        ExpressionConstant::create( Value( 5 ) );
                // The constant is copied out as is.
                assertBinaryEqual( constify( BSON_ARRAY( 5 ) ), toBsonArray( expression ) );
            }
        private:
            static BSONObj toBsonArray( const intrusive_ptr<Expression>& expression ) {
                BSONArrayBuilder bab;
                bab << expression->serialize();
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
                assertBinaryEqual( fromjson( "{}" ),
                                   toBson( expression->evaluate( Document() ) ) );
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
                assertBinaryEqual( fromjson( "{}" ),
                                   toBson( expression->evaluate
                                          ( fromBson( fromjson( "{a:null}" ) ) ) ) );
            }
        };
        
        /** Target field parent is undefined. */
        class NestedBelowUndefined {
        public:
            void run() {
                intrusive_ptr<Expression> expression = ExpressionFieldPath::create( "a.b" );
                assertBinaryEqual( fromjson( "{}" ),
                                   toBson( expression->evaluate
                                          ( fromBson( fromjson( "{a:undefined}" ) ) ) ) );
            }
        };

        /** Target field parent is missing. */
        class NestedBelowMissing {
        public:
            void run() {
                intrusive_ptr<Expression> expression = ExpressionFieldPath::create( "a.b" );
                assertBinaryEqual( fromjson( "{}" ),
                                   toBson( expression->evaluate
                                          ( fromBson( fromjson( "{z:1}" ) ) ) ) );
            }
        };
        
        /** Target field parent is an integer. */
        class NestedBelowInt {
        public:
            void run() {
                intrusive_ptr<Expression> expression = ExpressionFieldPath::create( "a.b" );
                assertBinaryEqual( fromjson( "{}" ),
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
                assertBinaryEqual( fromjson( "{}" ),
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
                assertBinaryEqual( fromjson( "{'':[]}" ),
                                   toBson( expression->evaluate
                                          ( fromBson( fromjson( "{a:[null]}" ) ) ) ) );
            }            
        };
        
        /** Target field within an array containing undefined. */
        class NestedBelowArrayWithUndefined {
        public:
            void run() {
                intrusive_ptr<Expression> expression = ExpressionFieldPath::create( "a.b" );
                assertBinaryEqual( fromjson( "{'':[]}" ),
                                   toBson( expression->evaluate
                                          ( fromBson( fromjson( "{a:[undefined]}" ) ) ) ) );
            }            
        };
        
        /** Target field within an array containing an integer. */
        class NestedBelowArrayWithInt {
        public:
            void run() {
                intrusive_ptr<Expression> expression = ExpressionFieldPath::create( "a.b" );
                assertBinaryEqual( fromjson( "{'':[]}" ),
                                   toBson( expression->evaluate
                                          ( fromBson( fromjson( "{a:[1]}" ) ) ) ) );
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
                assertBinaryEqual( fromjson( "{'':[9,20]}" ),
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
                assertBinaryEqual(BSON("foo" << "$a.b.c"), BSON("foo" << expression->serialize()));
            }
        };

        /** Add to a BSONArray. */
        class AddToBsonArray {
        public:
            void run() {
                intrusive_ptr<Expression> expression = ExpressionFieldPath::create( "a.b.c" );
                BSONArrayBuilder bab;
                bab << expression->serialize();
                assertBinaryEqual( BSON_ARRAY( "$a.b.c" ), bab.arr() );
            }
        };
        
    } // namespace FieldPath

    namespace FieldRange {

        // Much of ExpressionFieldRange's functionality is not reachable in mongo 2.2. and some of
        // it does not work properly.  These tests generally focus portions of ExpressionFieldRange
        // that are reachable in mongo 2.2.

        class CheckResultBase {
        public:
            virtual ~CheckResultBase() {
            }
            void run() {
                intrusive_ptr<ExpressionFieldRange> expression =
                        ExpressionFieldRange::create( mongo::ExpressionFieldPath::create( "a" ),
                                                      compareOp(), valueFromBson( value() ) );
                ASSERT_EQUALS( constify( expectedSpec() ), expressionToBson( expression ) );
                ASSERT_EQUALS( toBson( expectedResult() ? Value(true) : Value(false) ),
                               toBson( expression->evaluate( fromBson( sourceDocument() ) ) ) );
            }
        protected:
            virtual Expression::CmpOp compareOp() = 0;
            virtual BSONObj expectedSpec() = 0;
            virtual BSONObj value() = 0;
            virtual BSONObj sourceDocument() = 0;
            virtual bool expectedResult() = 0;
        };

        class EqBase : public CheckResultBase {
            Expression::CmpOp compareOp() { return Expression::EQ; }
            BSONObj expectedSpec() { return BSON( "$eq" << BSON_ARRAY( "$a" << 1 ) ); }
            BSONObj value() { return BSON( "" << 1 ); }
        };

        /** $eq operator with 'a' < value. */
        class EqLt : public EqBase {
            BSONObj sourceDocument() { return BSON( "a" << 0 ); }
            bool expectedResult() { return false; }
        };

        /** $eq operator with 'a' == value. */
        class EqEq : public EqBase {
            BSONObj sourceDocument() { return BSON( "a" << 1 ); }
            bool expectedResult() { return true; }
        };
        
        /** $eq operator with 'a' > value. */
        class EqGt : public EqBase {
            BSONObj sourceDocument() { return BSON( "a" << 2 ); }
            bool expectedResult() { return false; }
        };
        
        class LtBase : public CheckResultBase {
            Expression::CmpOp compareOp() { return Expression::LT; }
            BSONObj expectedSpec() { return BSON( "$lt" << BSON_ARRAY( "$a" << "y" ) ); }
            BSONObj value() { return BSON( "" << "y" ); }
        };
        
        /** $lt operator with 'a' < value. */
        class LtLt : public LtBase {
            BSONObj sourceDocument() { return BSON( "a" << "x" ); }
            bool expectedResult() { return true; }
        };
        
        /** $lt operator with 'a' == value. */
        class LtEq : public LtBase {
            BSONObj sourceDocument() { return BSON( "a" << "y" ); }
            bool expectedResult() { return false; }
        };
        
        /** $lt operator with 'a' > value. */
        class LtGt : public LtBase {
            BSONObj sourceDocument() { return BSON( "a" << "z" ); }
            bool expectedResult() { return false; }
        };
        
        class LteBase : public CheckResultBase {
            Expression::CmpOp compareOp() { return Expression::LTE; }
            BSONObj expectedSpec() { return BSON( "$lte" << BSON_ARRAY( "$a" << 1.1 ) ); }
            BSONObj value() { return BSON( "" << 1.1 ); }
        };
        
        /** $lte operator with 'a' < value. */
        class LteLt : public LteBase {
            BSONObj sourceDocument() { return BSON( "a" << 1.0 ); }
            bool expectedResult() { return true; }
        };
        
        /** $lte operator with 'a' == value. */
        class LteEq : public LteBase {
            BSONObj sourceDocument() { return BSON( "a" << 1.1 ); }
            bool expectedResult() { return true; }
        };
        
        /** $lte operator with 'a' > value. */
        class LteGt : public LteBase {
            BSONObj sourceDocument() { return BSON( "a" << 1.2 ); }
            bool expectedResult() { return false; }
        };
        
        class GtBase : public CheckResultBase {
            Expression::CmpOp compareOp() { return Expression::GT; }
            BSONObj expectedSpec() { return BSON( "$gt" << BSON_ARRAY( "$a" << 100 ) ); }
            BSONObj value() { return BSON( "" << 100 ); }
        };
        
        /** $gt operator with 'a' < value. */
        class GtLt : public GtBase {
            BSONObj sourceDocument() { return BSON( "a" << 50 ); }
            bool expectedResult() { return false; }
        };
        
        /** $gt operator with 'a' == value. */
        class GtEq : public GtBase {
            BSONObj sourceDocument() { return BSON( "a" << 100 ); }
            bool expectedResult() { return false; }
        };
        
        /** $gt operator with 'a' > value. */
        class GtGt : public GtBase {
            BSONObj sourceDocument() { return BSON( "a" << 150 ); }
            bool expectedResult() { return true; }
        };
        
        class GteBase : public CheckResultBase {
            Expression::CmpOp compareOp() { return Expression::GTE; }
            BSONObj expectedSpec() { return BSON( "$gte" << BSON_ARRAY( "$a" << "abc" ) ); }
            BSONObj value() { return BSON( "" << "abc" ); }
        };
        
        /** $gte operator with 'a' < value. */
        class GteLt : public GteBase {
            BSONObj sourceDocument() { return BSON( "a" << "a" ); }
            bool expectedResult() { return false; }
        };
        
        /** $gte operator with 'a' == value. */
        class GteEq : public GteBase {
            BSONObj sourceDocument() { return BSON( "a" << "abc" ); }
            bool expectedResult() { return true; }
        };
        
        /** $gte operator with 'a' > value. */
        class GteGt : public GteBase {
            BSONObj sourceDocument() { return BSON( "a" << "abcd" ); }
            bool expectedResult() { return true; }
        };

        /** The FieldRange's FieldPath is a dependency. */
        class Dependencies {
        public:
            void run() {
                intrusive_ptr<ExpressionFieldRange> expression =
                        ExpressionFieldRange::create( mongo::ExpressionFieldPath::create( "a.b.c" ),
                                                      Expression::EQ, Value(0) );
                set<string> dependencies;
                expression->addDependencies( dependencies );
                ASSERT_EQUALS( 1U, dependencies.size() );
                ASSERT_EQUALS( 1U, dependencies.count( "a.b.c" ) );
            }
        };

        /** Comparison is performed for multikey values rather than set-containment. */
        class Multikey {
        public:
            void run() {
                intrusive_ptr<ExpressionFieldRange> expression =
                        ExpressionFieldRange::create( mongo::ExpressionFieldPath::create( "a" ),
                                                      Expression::EQ, Value(0) );
                Document document =
                        fromBson( BSON( "a" << BSON_ARRAY( 1 << 0 << 2 ) ) );
                ASSERT_EQUALS(expression->evaluate(document), Value(false));
            }
        };
        
    } // namespace FieldRange

    namespace Nary {

        /** A dummy child of ExpressionNary used for testing. */
        class Testable : public ExpressionNary {
        public:
            virtual Value evaluateInternal(const Variables& vars) const {
                // Just put all the values in a list.  This is not associative/commutative so
                // the results will change if a factory is provided and operations are reordered.
                vector<Value> values;
                for( ExpressionVector::const_iterator i = vpOperand.begin(); i != vpOperand.end();
                     ++i ) {
                    values.push_back( (*i)->evaluateInternal(vars) );
                }
                return Value( values );
            }
            virtual const char* getOpName() const { return "$testable"; }
            virtual bool isAssociativeAndCommutative() const {
                return _isAssociativeAndCommutative;
            }
            static intrusive_ptr<Testable> create( bool associativeAndCommutative = false ) {
                return new Testable(associativeAndCommutative);
            }
            static intrusive_ptr<ExpressionNary> factory() {
                return new Testable(true);
            }
            static intrusive_ptr<Testable> createFromOperands( const BSONArray& operands,
                                                               bool haveFactory = false ) {
                intrusive_ptr<Testable> testable = create( haveFactory );
                BSONObjIterator i( operands );
                while( i.more() ) {
                    BSONElement element = i.next();
                    testable->addOperand( Expression::parseOperand( element ) );
                }
                return testable;
            }
            void assertContents( const BSONArray& expectedContents ) {
                ASSERT_EQUALS( constify( BSON( "$testable" << expectedContents ) ), expressionToBson( this ) );
            }

        private:
            Testable(bool isAssociativeAndCommutative)
                : _isAssociativeAndCommutative(isAssociativeAndCommutative)
            {}
            bool _isAssociativeAndCommutative;
        };

        /** Adding operands to the expression. */
        class AddOperand {
        public:
            void run() {
                intrusive_ptr<Testable> testable = Testable::create();
                testable->addOperand( ExpressionConstant::create( Value( 9 ) ) );
                testable->assertContents( BSON_ARRAY( 9 ) );
                testable->addOperand( ExpressionFieldPath::create( "ab.c" ) );
                testable->assertContents( BSON_ARRAY( 9 << "$ab.c" ) );
            }
        };

        /** Dependencies of the expression. */
        class Dependencies {
        public:
            void run() {
                intrusive_ptr<Testable> testable = Testable::create();

                // No arguments.
                assertDependencies( BSONArray(), testable );

                // Add a constant argument.
                testable->addOperand( ExpressionConstant::create( Value( 1 ) ) );
                assertDependencies( BSONArray(), testable );

                // Add a field path argument.
                testable->addOperand( ExpressionFieldPath::create( "ab.c" ) );
                assertDependencies( BSON_ARRAY( "ab.c" ), testable );

                // Add an object expression.
                BSONObj spec = BSON( "" << BSON( "a" << "$x" << "q" << "$r" ) );
                BSONElement specElement = spec.firstElement();
                Expression::ObjectCtx ctx( Expression::ObjectCtx::DOCUMENT_OK );
                testable->addOperand( Expression::parseObject( &specElement, &ctx ) );
                assertDependencies( BSON_ARRAY( "ab.c" << "r" << "x" ), testable );
            }
        private:
            void assertDependencies( const BSONArray& expectedDependencies,
                                     const intrusive_ptr<Expression>& expression ) {
                set<string> dependencies;
                expression->addDependencies( dependencies );
                BSONArrayBuilder dependenciesBson;
                for( set<string>::const_iterator i = dependencies.begin(); i != dependencies.end();
                     ++i ) {
                    dependenciesBson << *i;
                }
                ASSERT_EQUALS( expectedDependencies, dependenciesBson.arr() );
            }                
        };

        /** Serialize to an object. */
        class AddToBsonObj {
        public:
            void run() {
                intrusive_ptr<Testable> testable = Testable::create();
                testable->addOperand( ExpressionConstant::create( Value( 5 ) ) );
                ASSERT_EQUALS(BSON("foo" << BSON("$testable" << BSON_ARRAY(BSON("$const" << 5)))),
                              BSON("foo" << testable->serialize()));
            }
        };

        /** Serialize to an array. */
        class AddToBsonArray {
        public:
            void run() {
                intrusive_ptr<Testable> testable = Testable::create();
                testable->addOperand( ExpressionConstant::create( Value( 5 ) ) );
                ASSERT_EQUALS(constify(BSON_ARRAY(BSON("$testable" << BSON_ARRAY(5)))),
                               BSON_ARRAY(testable->serialize()));
            }
        };

        /** One operand is optimized to a constant, while another is left as is. */
        class OptimizeOneOperand {
        public:
            void run() {
                BSONArray spec = BSON_ARRAY( BSON( "$and" << BSONArray() ) << "$abc" );
                intrusive_ptr<Testable> testable = Testable::createFromOperands( spec );
                testable->assertContents( spec );
                ASSERT( testable == testable->optimize() );
                testable->assertContents( BSON_ARRAY( true << "$abc" ) );
            }
        };
        
        /** All operands are constants, and the operator is evaluated with them. */
        class EvaluateAllConstantOperands {
        public:
            void run() {
                BSONArray spec = BSON_ARRAY( 1 << 2 );
                intrusive_ptr<Testable> testable = Testable::createFromOperands( spec );
                testable->assertContents( spec );
                intrusive_ptr<Expression> optimized = testable->optimize();
                ASSERT( testable != optimized );
                ASSERT_EQUALS( BSON( "$const" << BSON_ARRAY( 1 << 2 ) ),
                               expressionToBson( optimized ) );
            }
        };

        class NoFactoryOptimizeBase {
        public:
            virtual ~NoFactoryOptimizeBase() {
            }
            void run() {
                intrusive_ptr<Testable> testable = createTestable();
                // Without factory optimization, optimization will not produce a new expression.
                ASSERT( testable == testable->optimize() );
            }
        protected:
            virtual intrusive_ptr<Testable> createTestable() = 0;
        };

        /** A string constant prevents factory optimization. */
        class StringConstant : public NoFactoryOptimizeBase {
            intrusive_ptr<Testable> createTestable() {
                return Testable::createFromOperands( BSON_ARRAY( "abc" << "def" << "$path" ),
                                                     true );
            }
        };

        /** A single (instead of multiple) constant prevents optimization.  SERVER-6192 */
        class SingleConstant : public NoFactoryOptimizeBase {
            intrusive_ptr<Testable> createTestable() {
                return Testable::createFromOperands( BSON_ARRAY( 55 << "$path" ), true );
            }
        };

        /** Factory optimization is not used without a factory. */
        class NoFactory : public NoFactoryOptimizeBase {
            intrusive_ptr<Testable> createTestable() {
                return Testable::createFromOperands( BSON_ARRAY( 55 << 66 << "$path" ), false );
            }
        };

        /** Factory optimization separates constant from non constant expressions. */
        class FactoryOptimize {
        public:
            void run() {
                intrusive_ptr<Testable> testable =
                        Testable::createFromOperands( BSON_ARRAY( 55 << 66 << "$path" ), true );
                intrusive_ptr<Expression> optimized = testable->optimize();
                // The constant expressions are evaluated separately and placed at the end.
                ASSERT_EQUALS( constify( BSON( "$testable"
                                         << BSON_ARRAY( "$path" << BSON_ARRAY( 55 << 66 ) ) ) ),
                               expressionToBson( optimized ) );
            }
        };

        /** Factory optimization flattens nested operators of the same type. */
        class FlattenOptimize {
        public:
            void run() {
                intrusive_ptr<Testable> testable =
                        Testable::createFromOperands
                        ( BSON_ARRAY( 55 << "$path" <<
                                      // $and has a factory, but it's a different factory from
                                      // $testable.
                                      BSON( "$add" << BSON_ARRAY( 5 << 6 << "$q" ) ) <<
                                      66 ),
                          true );
                // Add a nested $testable operand.
                testable->addOperand
                        ( Testable::createFromOperands
                          ( BSON_ARRAY( 99 << 100 << "$another_path" ), true ) );
                intrusive_ptr<Expression> optimized = testable->optimize();
                ASSERT_EQUALS
                        ( constify( BSON( "$testable" <<
                                BSON_ARRAY( // non constant parts
                                            "$path" <<
                                            BSON( "$add" << BSON_ARRAY( "$q" << 11 ) ) <<
                                            "$another_path" <<
                                            // constant part last
                                            BSON_ARRAY( 55 << 66 << BSON_ARRAY( 99 << 100 ) ) ) ) ),
                          expressionToBson( optimized ) );
            }
        };

        /** Three layers of factory optimization are flattened. */
        class FlattenThreeLayers {
        public:
            void run() {
                intrusive_ptr<Testable> top =
                        Testable::createFromOperands( BSON_ARRAY( 1 << 2 << "$a" ), true );
                intrusive_ptr<Testable> nested =
                        Testable::createFromOperands( BSON_ARRAY( 3 << 4 << "$b" ), true );
                nested->addOperand
                        ( Testable::createFromOperands( BSON_ARRAY( 5 << 6 << "$c" ), true ) );
                top->addOperand( nested );
                intrusive_ptr<Expression> optimized = top->optimize();
                ASSERT_EQUALS
                        ( constify( BSON( "$testable" <<
                                BSON_ARRAY( "$a" << "$b" << "$c" <<
                                            BSON_ARRAY( 1 << 2 <<
                                                        BSON_ARRAY( 3 << 4 <<
                                                                    BSON_ARRAY( 5 << 6 ) ) ) ) ) ),
                           expressionToBson( optimized ) );
            }
        };
        
    } // namespace Nary

    namespace Object {

        class Base {
        protected:
            void assertDependencies( const BSONArray& expectedDependencies,
                                     const intrusive_ptr<ExpressionObject>& expression,
                                     bool includePath = true ) const {
                set<string> dependencies;
                vector<string> path;
                expression->addDependencies( dependencies, includePath ? &path : 0 );
                BSONArrayBuilder bab;
                for( set<string>::const_iterator i = dependencies.begin(); i != dependencies.end();
                    ++i ) {
                    bab << *i;
                }
                ASSERT_EQUALS( expectedDependencies, bab.arr() );
            }            
        };

        class ExpectedResultBase : public Base {
        public:
            virtual ~ExpectedResultBase() {
            }
            void run() {
                _expression = ExpressionObject::createRoot();
                prepareExpression();
                Document document = fromBson( source() );
                MutableDocument result;
                expression()->addToDocument( result, document, Variables(document) );
                assertBinaryEqual( expected(), toBson( result.freeze() ) );
                assertDependencies( expectedDependencies(), _expression );
                ASSERT_EQUALS( expectedBsonRepresentation(), expressionToBson( _expression ) );
                ASSERT_EQUALS( expectedIsSimple(), _expression->isSimple() );
            }
        protected:
            intrusive_ptr<ExpressionObject> expression() { return _expression; }
            virtual BSONObj source() { return BSON( "_id" << 0 << "a" << 1 << "b" << 2 ); }
            virtual void prepareExpression() = 0;
            virtual BSONObj expected() = 0;
            virtual BSONArray expectedDependencies() = 0;
            virtual BSONObj expectedBsonRepresentation() = 0;
            virtual bool expectedIsSimple() { return true; }
        private:
            intrusive_ptr<ExpressionObject> _expression;
        };

        /** Empty object spec. */
        class Empty : public ExpectedResultBase {
        public:
            void prepareExpression() {}
            BSONObj expected() { return BSON( "_id" << 0 ); }
            BSONArray expectedDependencies() { return BSON_ARRAY( "_id" ); }
            BSONObj expectedBsonRepresentation() { return BSONObj(); }
        };

        /** Include 'a' field only. */
        class Include : public ExpectedResultBase {
        public:
            void prepareExpression() { expression()->includePath( "a" ); }
            BSONObj expected() { return BSON( "_id" << 0 << "a" << 1 ); }
            BSONArray expectedDependencies() { return BSON_ARRAY( "_id" << "a" ); }
            BSONObj expectedBsonRepresentation() {
                return BSON( "a" << true );
            }
        };

        /** Cannot include missing 'a' field. */
        class MissingInclude : public ExpectedResultBase {
        public:
            virtual BSONObj source() { return BSON( "_id" << 0 << "b" << 2 ); }
            void prepareExpression() { expression()->includePath( "a" ); }
            BSONObj expected() { return BSON( "_id" << 0 ); }
            BSONArray expectedDependencies() { return BSON_ARRAY( "_id" << "a" ); }
            BSONObj expectedBsonRepresentation() {
                return BSON( "a" << true );
            }
        };
        
        /** Include '_id' field only. */
        class IncludeId : public ExpectedResultBase {
        public:
            void prepareExpression() { expression()->includePath( "_id" ); }
            BSONObj expected() { return BSON( "_id" << 0 ); }            
            BSONArray expectedDependencies() { return BSON_ARRAY( "_id" ); }
            BSONObj expectedBsonRepresentation() {
                return BSON( "_id" << true );
            }
        };

        /** Exclude '_id' field. */
        class ExcludeId : public ExpectedResultBase {
        public:
            void prepareExpression() {
                expression()->includePath( "b" );
                expression()->excludeId( true );
            }
            BSONObj expected() { return BSON( "b" << 2 ); }
            BSONArray expectedDependencies() { return BSON_ARRAY( "b" ); }
            BSONObj expectedBsonRepresentation() {
                return BSON( "_id" << false << "b" << true );
            }
        };

        /** Result order based on source document field order, not inclusion spec field order. */
        class SourceOrder : public ExpectedResultBase {
        public:
            void prepareExpression() {
                expression()->includePath( "b" );
                expression()->includePath( "a" );
            }
            BSONObj expected() { return source(); }            
            BSONArray expectedDependencies() { return BSON_ARRAY( "_id" << "a" << "b" ); }
            BSONObj expectedBsonRepresentation() {
                return BSON( "b" << true << "a" << true );
            }
        };

        /** Include a nested field. */
        class IncludeNested : public ExpectedResultBase {
        public:
            void prepareExpression() { expression()->includePath( "a.b" ); }
            BSONObj expected() { return BSON( "_id" << 0 << "a" << BSON( "b" << 5 ) ); }
            BSONObj source() {
                return BSON( "_id" << 0 << "a" << BSON( "b" << 5 << "c" << 6 ) << "z" << 2 );
            }
            BSONArray expectedDependencies() { return BSON_ARRAY( "_id" << "a.b" ); }
            BSONObj expectedBsonRepresentation() {
                return BSON( "a" << BSON( "b" << true ) );
            }
        };

        /** Include two nested fields. */
        class IncludeTwoNested : public ExpectedResultBase {
        public:
            void prepareExpression() {
                expression()->includePath( "a.b" );
                expression()->includePath( "a.c" );
            }
            BSONObj expected() { return BSON( "_id" << 0 << "a" << BSON( "b" << 5 << "c" << 6 ) ); }
            BSONObj source() {
                return BSON( "_id" << 0 << "a" << BSON( "b" << 5 << "c" << 6 ) << "z" << 2 );
            }
            BSONArray expectedDependencies() { return BSON_ARRAY( "_id" << "a.b" << "a.c" ); }
            BSONObj expectedBsonRepresentation() {
                return BSON( "a" << BSON( "b" << true << "c" << true ) );
            }
        };
        
        /** Include two fields nested within different parents. */
        class IncludeTwoParentNested : public ExpectedResultBase {
        public:
            void prepareExpression() {
                expression()->includePath( "a.b" );
                expression()->includePath( "c.d" );
            }
            BSONObj expected() {
                return BSON( "_id" << 0 << "a" << BSON( "b" << 5 ) << "c" << BSON( "d" << 6 ) );
            }
            BSONObj source() {
                return BSON( "_id" << 0 << "a" << BSON( "b" << 5 )
                             << "c" << BSON( "d" << 6 ) << "z" << 2 );
            }
            BSONArray expectedDependencies() { return BSON_ARRAY( "_id" << "a.b" << "c.d" ); }
            BSONObj expectedBsonRepresentation() {
                return BSON( "a" << BSON( "b" << true ) << "c" << BSON( "d" << true ) );
            }
        };
        
        /** Attempt to include a missing nested field. */
        class IncludeMissingNested : public ExpectedResultBase {
        public:
            void prepareExpression() { expression()->includePath( "a.b" ); }
            BSONObj expected() { return BSON( "_id" << 0 << "a" << BSONObj() ); }
            BSONObj source() {
                return BSON( "_id" << 0 << "a" << BSON( "c" << 6 ) << "z" << 2 );
            }
            BSONArray expectedDependencies() { return BSON_ARRAY( "_id" << "a.b" ); }
            BSONObj expectedBsonRepresentation() {
                return BSON( "a" << BSON( "b" << true ) );
            }
        };
        
        /** Attempt to include a nested field within a non object. */
        class IncludeNestedWithinNonObject : public ExpectedResultBase {
        public:
            void prepareExpression() { expression()->includePath( "a.b" ); }
            BSONObj expected() { return BSON( "_id" << 0 ); }
            BSONObj source() {
                return BSON( "_id" << 0 << "a" << 2 << "z" << 2 );
            }
            BSONArray expectedDependencies() { return BSON_ARRAY( "_id" << "a.b" ); }
            BSONObj expectedBsonRepresentation() {
                return BSON( "a" << BSON( "b" << true ) );
            }
        };
        
        /** Include a nested field within an array. */
        class IncludeArrayNested : public ExpectedResultBase {
        public:
            void prepareExpression() { expression()->includePath( "a.b" ); }
            BSONObj expected() { return fromjson( "{_id:0,a:[{b:5},{b:2},{}]}" ); }
            BSONObj source() {
                return fromjson( "{_id:0,a:[{b:5,c:6},{b:2,c:9},{c:7},[],2],z:1}" );
            }
            BSONArray expectedDependencies() { return BSON_ARRAY( "_id" << "a.b" ); }
            BSONObj expectedBsonRepresentation() {
                return BSON( "a" << BSON( "b" << true ) );
            }
        };
        
        /** Don't include not root '_id' field implicitly. */
        class ExcludeNonRootId : public ExpectedResultBase {
        public:
            virtual BSONObj source() {
                return BSON( "_id" << 0 << "a" << BSON( "_id" << 1 << "b" << 1 ) );
            }
            void prepareExpression() { expression()->includePath( "a.b" ); }
            BSONObj expected() { return BSON( "_id" << 0 << "a" << BSON( "b" << 1 ) ); }
            BSONArray expectedDependencies() { return BSON_ARRAY( "_id" << "a.b" ); }
            BSONObj expectedBsonRepresentation() {
                return BSON( "a" << BSON( "b" << true ) );
            }
        };

        /** Project a computed expression. */
        class Computed : public ExpectedResultBase {
        public:
            virtual BSONObj source() {
                return BSON( "_id" << 0 );
            }
            void prepareExpression() {
                expression()->addField( mongo::FieldPath( "a" ),
                                        ExpressionConstant::create( Value( 5 ) ) );
            }
            BSONObj expected() { return BSON( "_id" << 0 << "a" << 5 ); }
            BSONArray expectedDependencies() { return BSON_ARRAY( "_id" ); }
            BSONObj expectedBsonRepresentation() {
                return BSON( "a" << BSON( "$const" << 5 ) );
            }
            bool expectedIsSimple() { return false; }
        };
        
        /** Project a computed expression replacing an existing field. */
        class ComputedReplacement : public Computed {
            virtual BSONObj source() {
                return BSON( "_id" << 0 << "a" << 99 );
            }
        };
        
        /** An undefined value is passed through */
        class ComputedUndefined : public ExpectedResultBase {
        public:
            virtual BSONObj source() {
                return BSON( "_id" << 0 );
            }
            void prepareExpression() {
                expression()->addField( mongo::FieldPath( "a" ),
                                        ExpressionConstant::create( Value(BSONUndefined) ) );
            }
            BSONObj expected() { return BSON( "_id" << 0 << "a" << BSONUndefined); }
            BSONArray expectedDependencies() { return BSON_ARRAY( "_id" ); }
            BSONObj expectedBsonRepresentation() {
                return fromjson( "{a:{$const:undefined}}" );
            }
            bool expectedIsSimple() { return false; }
        };
        
        /** Project a computed expression replacing an existing field with Undefined. */
        class ComputedUndefinedReplacement : public ComputedUndefined {
            virtual BSONObj source() {
                return BSON( "_id" << 0 << "a" << 99 );
            }
        };
        
        /** A null value is projected. */
        class ComputedNull : public ExpectedResultBase {
        public:
            virtual BSONObj source() {
                return BSON( "_id" << 0 );
            }
            void prepareExpression() {
                expression()->addField( mongo::FieldPath( "a" ),
                                        ExpressionConstant::create( Value(BSONNULL) ) );
            }
            BSONObj expected() { return BSON( "_id" << 0 << "a" << BSONNULL ); }
            BSONArray expectedDependencies() { return BSON_ARRAY( "_id" ); }
            BSONObj expectedBsonRepresentation() {
                return BSON( "a" << BSON( "$const" << BSONNULL ) );
            }
            bool expectedIsSimple() { return false; }
        };
        
        /** A nested value is projected. */
        class ComputedNested : public ExpectedResultBase {
        public:
            virtual BSONObj source() { return BSON( "_id" << 0 ); }
            void prepareExpression() {
                expression()->addField( mongo::FieldPath( "a.b" ),
                                        ExpressionConstant::create( Value( 5 ) ) );
            }
            BSONObj expected() { return BSON( "_id" << 0 << "a" << BSON( "b" << 5 ) ); }
            BSONArray expectedDependencies() { return BSON_ARRAY( "_id" ); }
            BSONObj expectedBsonRepresentation() {
                return BSON( "a" << BSON( "b" << BSON( "$const" << 5 ) ) );
            }
            bool expectedIsSimple() { return false; }
        };
        
        /** A field path is projected. */
        class ComputedFieldPath : public ExpectedResultBase {
        public:
            virtual BSONObj source() { return BSON( "_id" << 0 << "x" << 4 ); }
            void prepareExpression() {
                expression()->addField( mongo::FieldPath( "a" ),
                                        ExpressionFieldPath::create( "x" ) );
            }
            BSONObj expected() { return BSON( "_id" << 0 << "a" << 4 ); }
            BSONArray expectedDependencies() { return BSON_ARRAY( "_id" << "x" ); }
            BSONObj expectedBsonRepresentation() { return BSON( "a" << "$x" ); }
            bool expectedIsSimple() { return false; }
        };
        
        /** A nested field path is projected. */
        class ComputedNestedFieldPath : public ExpectedResultBase {
        public:
            virtual BSONObj source() { return BSON( "_id" << 0 << "x" << BSON( "y" << 4 ) ); }
            void prepareExpression() {
                expression()->addField( mongo::FieldPath( "a.b" ),
                                        ExpressionFieldPath::create( "x.y" ) );
            }
            BSONObj expected() { return BSON( "_id" << 0 << "a" << BSON( "b" << 4 ) ); }
            BSONArray expectedDependencies() { return BSON_ARRAY( "_id" << "x.y" ); }
            BSONObj expectedBsonRepresentation() { return BSON( "a" << BSON( "b" << "$x.y" ) ); }
            bool expectedIsSimple() { return false; }
        };
        
        /** An empty subobject expression for a missing field is not projected. */
        class EmptyNewSubobject : public ExpectedResultBase {
        public:
            virtual BSONObj source() {
                return BSON( "_id" << 0 );
            }
            void prepareExpression() {
                // Create a sub expression returning an empty object.
                intrusive_ptr<ExpressionObject> subExpression = ExpressionObject::create();
                subExpression->addField( mongo::FieldPath( "b" ),
                                         ExpressionFieldPath::create( "a.b" ) );
                expression()->addField( mongo::FieldPath( "a" ), subExpression );
            }
            BSONObj expected() { return BSON( "_id" << 0 ); }
            BSONArray expectedDependencies() { return BSON_ARRAY( "_id" << "a.b"); }
            BSONObj expectedBsonRepresentation() {
                return fromjson( "{a:{b:'$a.b'}}" );
            }
            bool expectedIsSimple() { return false; }
        };
        
        /** A non empty subobject expression for a missing field is projected. */
        class NonEmptyNewSubobject : public ExpectedResultBase {
        public:
            virtual BSONObj source() {
                return BSON( "_id" << 0 );
            }
            void prepareExpression() {
                // Create a sub expression returning an empty object.
                intrusive_ptr<ExpressionObject> subExpression = ExpressionObject::create();
                subExpression->addField( mongo::FieldPath( "b" ),
                                         ExpressionConstant::create( Value( 6 ) ) );
                expression()->addField( mongo::FieldPath( "a" ), subExpression );
            }
            BSONObj expected() { return BSON( "_id" << 0 << "a" << BSON( "b" << 6 ) ); }
            BSONArray expectedDependencies() { return BSON_ARRAY( "_id" ); }
            BSONObj expectedBsonRepresentation() {
                return fromjson( "{a:{b:{$const:6}}}" );
            }
            bool expectedIsSimple() { return false; }
        };

        /** Two computed fields within a common parent. */
        class AdjacentDottedComputedFields : public ExpectedResultBase {
        public:
            virtual BSONObj source() {
                return BSON( "_id" << 0 );
            }
            void prepareExpression() {
                expression()->addField( mongo::FieldPath( "a.b" ),
                                        ExpressionConstant::create( Value( 6 ) ) );
                expression()->addField( mongo::FieldPath( "a.c" ),
                                        ExpressionConstant::create( Value( 7 ) ) );
            }
            BSONObj expected() { return BSON( "_id" << 0 << "a" << BSON( "b" << 6 << "c" << 7 ) ); }
            BSONArray expectedDependencies() { return BSON_ARRAY( "_id" ); }
            BSONObj expectedBsonRepresentation() {
                return fromjson( "{a:{b:{$const:6},c:{$const:7}}}" );
            }
            bool expectedIsSimple() { return false; }
        };

        /** Two computed fields within a common parent, in one case dotted. */
        class AdjacentDottedAndNestedComputedFields : public AdjacentDottedComputedFields {
            void prepareExpression() {
                expression()->addField( mongo::FieldPath( "a.b" ),
                                        ExpressionConstant::create( Value( 6 ) ) );
                intrusive_ptr<ExpressionObject> subExpression = ExpressionObject::create();
                subExpression->addField( mongo::FieldPath( "c" ),
                                         ExpressionConstant::create( Value( 7 ) ) );
                expression()->addField( mongo::FieldPath( "a" ), subExpression );
            }
        };
        
        /** Two computed fields within a common parent, in another case dotted. */
        class AdjacentNestedAndDottedComputedFields : public AdjacentDottedComputedFields {
            void prepareExpression() {
                intrusive_ptr<ExpressionObject> subExpression = ExpressionObject::create();
                subExpression->addField( mongo::FieldPath( "b" ),
                                         ExpressionConstant::create( Value( 6 ) ) );
                expression()->addField( mongo::FieldPath( "a" ), subExpression );
                expression()->addField( mongo::FieldPath( "a.c" ),
                                        ExpressionConstant::create( Value( 7 ) ) );
            }
        };

        /** Two computed fields within a common parent, nested rather than dotted. */
        class AdjacentNestedComputedFields : public AdjacentDottedComputedFields {
            void prepareExpression() {
                intrusive_ptr<ExpressionObject> firstSubExpression = ExpressionObject::create();
                firstSubExpression->addField( mongo::FieldPath( "b" ),
                                              ExpressionConstant::create( Value( 6 ) ) );
                expression()->addField( mongo::FieldPath( "a" ), firstSubExpression );
                intrusive_ptr<ExpressionObject> secondSubExpression = ExpressionObject::create();
                secondSubExpression->addField( mongo::FieldPath( "c" ),
                                               ExpressionConstant::create
                                                ( Value( 7 ) ) );
                expression()->addField( mongo::FieldPath( "a" ), secondSubExpression );
            }            
        };

        /** Field ordering is preserved when nested fields are merged. */
        class AdjacentNestedOrdering : public ExpectedResultBase {
        public:
            virtual BSONObj source() {
                return BSON( "_id" << 0 );
            }
            void prepareExpression() {
                expression()->addField( mongo::FieldPath( "a.b" ),
                                        ExpressionConstant::create( Value( 6 ) ) );
                intrusive_ptr<ExpressionObject> subExpression = ExpressionObject::create();
                // Add field 'd' then 'c'.  Expect the same field ordering in the result doc.
                subExpression->addField( mongo::FieldPath( "d" ),
                                         ExpressionConstant::create( Value( 7 ) ) );
                subExpression->addField( mongo::FieldPath( "c" ),
                                         ExpressionConstant::create( Value( 8 ) ) );
                expression()->addField( mongo::FieldPath( "a" ), subExpression );
            }
            BSONObj expected() {
                return BSON( "_id" << 0 << "a" << BSON( "b" << 6 << "d" << 7 << "c" << 8 ) );
            }
            BSONArray expectedDependencies() { return BSON_ARRAY( "_id" ); }
            BSONObj expectedBsonRepresentation() {
                return fromjson( "{a:{b:{$const:6},d:{$const:7},c:{$const:8}}}" );
            }
            bool expectedIsSimple() { return false; }
        };

        /** Adjacent fields two levels deep. */
        class MultipleNestedFields : public ExpectedResultBase {
        public:
            virtual BSONObj source() {
                return BSON( "_id" << 0 );
            }
            void prepareExpression() {
                expression()->addField( mongo::FieldPath( "a.b.c" ),
                                        ExpressionConstant::create( Value( 6 ) ) );
                intrusive_ptr<ExpressionObject> bSubExpression = ExpressionObject::create();
                bSubExpression->addField( mongo::FieldPath( "d" ),
                                          ExpressionConstant::create( Value( 7 ) ) );
                intrusive_ptr<ExpressionObject> aSubExpression = ExpressionObject::create();
                aSubExpression->addField( mongo::FieldPath( "b" ), bSubExpression );
                expression()->addField( mongo::FieldPath( "a" ), aSubExpression );
            }
            BSONObj expected() {
                return BSON( "_id" << 0 << "a" << BSON( "b" << BSON( "c" << 6 << "d" << 7 ) ) );
            }
            BSONArray expectedDependencies() { return BSON_ARRAY( "_id" ); }
            BSONObj expectedBsonRepresentation() {
                return fromjson( "{a:{b:{c:{$const:6},d:{$const:7}}}}" );
            }
            bool expectedIsSimple() { return false; }
        };
        
        /** Two expressions cannot generate the same field. */
        class ConflictingExpressionFields : public Base {
        public:
            void run() {
                intrusive_ptr<ExpressionObject> expression = ExpressionObject::createRoot();
                expression->addField( mongo::FieldPath( "a" ),
                                      ExpressionConstant::create( Value( 5 ) ) );
                ASSERT_THROWS( expression->addField( mongo::FieldPath( "a" ), // Duplicate field.
                                                     ExpressionConstant::create
                                                      ( Value( 6 ) ) ),
                               UserException );
            }
        };        
        
        /** An expression field conflicts with an inclusion field. */
        class ConflictingInclusionExpressionFields : public Base {
        public:
            void run() {
                intrusive_ptr<ExpressionObject> expression = ExpressionObject::createRoot();
                expression->includePath( "a" );
                ASSERT_THROWS( expression->addField( mongo::FieldPath( "a" ),
                                                     ExpressionConstant::create
                                                      ( Value( 6 ) ) ),
                               UserException );
            }
        };        
        
        /** An inclusion field conflicts with an expression field. */
        class ConflictingExpressionInclusionFields : public Base {
        public:
            void run() {
                intrusive_ptr<ExpressionObject> expression = ExpressionObject::createRoot();
                expression->addField( mongo::FieldPath( "a" ),
                                      ExpressionConstant::create( Value( 5 ) ) );
                ASSERT_THROWS( expression->includePath( "a" ),
                               UserException );
            }
        };        
        
        /** An object expression conflicts with a constant expression. */
        class ConflictingObjectConstantExpressionFields : public Base {
        public:
            void run() {
                intrusive_ptr<ExpressionObject> expression = ExpressionObject::createRoot();
                intrusive_ptr<ExpressionObject> subExpression = ExpressionObject::create();
                subExpression->includePath( "b" );
                expression->addField( mongo::FieldPath( "a" ), subExpression );
                ASSERT_THROWS( expression->addField( mongo::FieldPath( "a.b" ),
                                                     ExpressionConstant::create
                                                      ( Value( 6 ) ) ),
                               UserException );
            }
        };        
        
        /** A constant expression conflicts with an object expression. */
        class ConflictingConstantObjectExpressionFields : public Base {
        public:
            void run() {
                intrusive_ptr<ExpressionObject> expression = ExpressionObject::createRoot();
                expression->addField( mongo::FieldPath( "a.b" ),
                                      ExpressionConstant::create( Value( 6 ) ) );
                intrusive_ptr<ExpressionObject> subExpression = ExpressionObject::create();
                subExpression->includePath( "b" );
                ASSERT_THROWS( expression->addField( mongo::FieldPath( "a" ), subExpression ),
                               UserException );
            }
        };        
        
        /** Two nested expressions cannot generate the same field. */
        class ConflictingNestedFields : public Base {
        public:
            void run() {
                intrusive_ptr<ExpressionObject> expression = ExpressionObject::createRoot();
                expression->addField( mongo::FieldPath( "a.b" ),
                                      ExpressionConstant::create( Value( 5 ) ) );
                ASSERT_THROWS( expression->addField( mongo::FieldPath( "a.b" ), // Duplicate field.
                                                     ExpressionConstant::create
                                                      ( Value( 6 ) ) ),
                               UserException );
            }
        };        
        
        /** An expression cannot be created for a subfield of another expression. */
        class ConflictingFieldAndSubfield : public Base {
        public:
            void run() {
                intrusive_ptr<ExpressionObject> expression = ExpressionObject::createRoot();
                expression->addField( mongo::FieldPath( "a" ),
                                      ExpressionConstant::create( Value( 5 ) ) );
                ASSERT_THROWS( expression->addField( mongo::FieldPath( "a.b" ),
                                                     ExpressionConstant::create
                                                      ( Value( 5 ) ) ),
                               UserException );
            }
        };

        /** An expression cannot be created for a nested field of another expression. */
        class ConflictingFieldAndNestedField : public Base {
        public:
            void run() {
                intrusive_ptr<ExpressionObject> expression = ExpressionObject::createRoot();
                expression->addField( mongo::FieldPath( "a" ),
                                      ExpressionConstant::create( Value( 5 ) ) );
                intrusive_ptr<ExpressionObject> subExpression = ExpressionObject::create();
                subExpression->addField( mongo::FieldPath( "b" ),
                                         ExpressionConstant::create( Value( 5 ) ) );
                ASSERT_THROWS( expression->addField( mongo::FieldPath( "a" ), subExpression ),
                               UserException );
            }
        };
        
        /** An expression cannot be created for a parent field of another expression. */
        class ConflictingSubfieldAndField : public Base {
        public:
            void run() {
                intrusive_ptr<ExpressionObject> expression = ExpressionObject::createRoot();
                expression->addField( mongo::FieldPath( "a.b" ),
                                      ExpressionConstant::create( Value( 5 ) ) );
                ASSERT_THROWS( expression->addField( mongo::FieldPath( "a" ),
                                                     ExpressionConstant::create
                                                      ( Value( 5 ) ) ),
                               UserException );
            }
        };
        
        /** An expression cannot be created for a parent of a nested field. */
        class ConflictingNestedFieldAndField : public Base {
        public:
            void run() {
                intrusive_ptr<ExpressionObject> expression = ExpressionObject::createRoot();
                intrusive_ptr<ExpressionObject> subExpression = ExpressionObject::create();
                subExpression->addField( mongo::FieldPath( "b" ),
                                         ExpressionConstant::create( Value( 5 ) ) );
                expression->addField( mongo::FieldPath( "a" ), subExpression );
                ASSERT_THROWS( expression->addField( mongo::FieldPath( "a" ),
                                                     ExpressionConstant::create
                                                      ( Value( 5 ) ) ),
                               UserException );
            }
        };
        
        /** Dependencies for non inclusion expressions. */
        class NonInclusionDependencies : public Base {
        public:
            void run() {
                intrusive_ptr<ExpressionObject> expression = ExpressionObject::createRoot();
                expression->addField( mongo::FieldPath( "a" ),
                                      ExpressionConstant::create( Value( 5 ) ) );
                assertDependencies( BSON_ARRAY( "_id" ), expression, true );
                assertDependencies( BSONArray(), expression, false );
                expression->addField( mongo::FieldPath( "b" ),
                                      ExpressionFieldPath::create( "c.d" ) );
                assertDependencies( BSON_ARRAY( "_id" << "c.d" ), expression, true );
                assertDependencies( BSON_ARRAY( "c.d" ), expression, false );
            }
        };

        /** Dependencies for inclusion expressions. */
        class InclusionDependencies : public Base {
        public:
            void run() {
                intrusive_ptr<ExpressionObject> expression = ExpressionObject::createRoot();
                expression->includePath( "a" );
                assertDependencies( BSON_ARRAY( "_id" << "a" ), expression, true );
                set<string> unused;
                // 'path' must be provided for inclusion expressions.
                ASSERT_THROWS( expression->addDependencies( unused ), UserException );
            }
        };

        /** Optimizing an object expression optimizes its sub expressions. */
        class Optimize : public Base {
        public:
            void run() {
                intrusive_ptr<ExpressionObject> expression = ExpressionObject::createRoot();
                // Add inclusion.
                expression->includePath( "a" );
                // Add non inclusion.
                intrusive_ptr<Expression> andExpr = new ExpressionAnd();
                expression->addField( mongo::FieldPath( "b" ), andExpr );
                expression->optimize();
                // Optimizing 'expression' optimizes its non inclusion sub expressions, while
                // inclusion sub expressions are passed through.
                ASSERT_EQUALS( BSON( "a" << true << "b" << BSON( "$const" << true ) ),
                               expressionToBson( expression ) );
            }
        };

        /** Serialize to a BSONObj. */
        class AddToBsonObj : public Base {
        public:
            void run() {
                intrusive_ptr<ExpressionObject> expression = ExpressionObject::createRoot();
                expression->addField( mongo::FieldPath( "a" ),
                                      ExpressionConstant::create( Value( 5 ) ) );
                ASSERT_EQUALS(constify(BSON("foo" << BSON("a" << 5))),
                              BSON("foo" << expression->serialize()));
            }
        };

        /** Serialize to a BSONObj, with constants represented by expressions. */
        class AddToBsonObjRequireExpression : public Base {
        public:
            void run() {
                intrusive_ptr<ExpressionObject> expression = ExpressionObject::createRoot();
                expression->addField( mongo::FieldPath( "a" ),
                                      ExpressionConstant::create( Value( 5 ) ) );
                ASSERT_EQUALS(BSON("foo" << BSON("a" << BSON("$const" << 5))),
                              BSON("foo" << expression->serialize()));
            }
        };
        
        /** Serialize to a BSONArray. */
        class AddToBsonArray : public Base {
        public:
            void run() {
                intrusive_ptr<ExpressionObject> expression = ExpressionObject::createRoot();
                expression->addField( mongo::FieldPath( "a" ),
                                      ExpressionConstant::create( Value( 5 ) ) );
                BSONArrayBuilder bab;
                bab << expression->serialize();
                ASSERT_EQUALS( constify( BSON_ARRAY( BSON( "a" << 5 ) ) ), bab.arr() );
            }
        };

        /**
         * evaluate() does not supply an inclusion document.  Inclusion spec'd fields are not
         * included.  (Inclusion specs are not generally expected/allowed in cases where evaluate
         * is called instead of addToDocument.)
         */
        class Evaluate : public Base {
        public:
            void run() {
                intrusive_ptr<ExpressionObject> expression = ExpressionObject::createRoot();
                expression->includePath( "a" );
                expression->addField( mongo::FieldPath( "b" ),
                                      ExpressionConstant::create( Value( 5 ) ) );
                expression->addField( mongo::FieldPath( "c" ),
                                      ExpressionFieldPath::create( "a" ) );
                ASSERT_EQUALS( BSON( "b" << 5 << "c" << 1 ),
                               toBson( expression->evaluate
                                       ( fromBson
                                         ( BSON( "_id" << 0 << "a" << 1 ) ) ).getDocument() ) );
            }
        };

    } // namespace Object

    namespace Or {
        
        class ExpectedResultBase {
        public:
            virtual ~ExpectedResultBase() {
            }
            void run() {
                BSONObj specObject = BSON( "" << spec() );
                BSONElement specElement = specObject.firstElement();
                intrusive_ptr<Expression> expression = Expression::parseOperand( specElement );
                ASSERT_EQUALS( constify( spec() ), expressionToBson( expression ) );
                ASSERT_EQUALS( BSON( "" << expectedResult() ),
                               toBson( expression->evaluate( fromBson( BSON( "a" << 1 ) ) ) ) );
                intrusive_ptr<Expression> optimized = expression->optimize();
                ASSERT_EQUALS( BSON( "" << expectedResult() ),
                               toBson( optimized->evaluate( fromBson( BSON( "a" << 1 ) ) ) ) );
            }
        protected:
            virtual BSONObj spec() = 0;
            virtual bool expectedResult() = 0;
        };
        
        class OptimizeBase {
        public:
            virtual ~OptimizeBase() {
            }
            void run() {
                BSONObj specObject = BSON( "" << spec() );
                BSONElement specElement = specObject.firstElement();
                intrusive_ptr<Expression> expression = Expression::parseOperand( specElement );
                ASSERT_EQUALS( constify( spec() ), expressionToBson( expression ) );
                intrusive_ptr<Expression> optimized = expression->optimize();
                ASSERT_EQUALS( expectedOptimized(), expressionToBson( optimized ) );
            }
        protected:
            virtual BSONObj spec() = 0;
            virtual BSONObj expectedOptimized() = 0;
        };
        
        class NoOptimizeBase : public OptimizeBase {
            BSONObj expectedOptimized() { return constify( spec() ); }
        };
        
        /** $or without operands. */
        class NoOperands : public ExpectedResultBase {
            BSONObj spec() { return BSON( "$or" << BSONArray() ); }
            bool expectedResult() { return false; }
        };
        
        /** $or passed 'true'. */
        class True : public ExpectedResultBase {
            BSONObj spec() { return BSON( "$or" << BSON_ARRAY( true ) ); }
            bool expectedResult() { return true; }
        };
        
        /** $or passed 'false'. */
        class False : public ExpectedResultBase {
            BSONObj spec() { return BSON( "$or" << BSON_ARRAY( false ) ); }
            bool expectedResult() { return false; }
        };
        
        /** $or passed 'true', 'true'. */
        class TrueTrue : public ExpectedResultBase {
            BSONObj spec() { return BSON( "$or" << BSON_ARRAY( true << true ) ); }
            bool expectedResult() { return true; }
        };
        
        /** $or passed 'true', 'false'. */
        class TrueFalse : public ExpectedResultBase {
            BSONObj spec() { return BSON( "$or" << BSON_ARRAY( true << false ) ); }
            bool expectedResult() { return true; }
        };
        
        /** $or passed 'false', 'true'. */
        class FalseTrue : public ExpectedResultBase {
            BSONObj spec() { return BSON( "$or" << BSON_ARRAY( false << true ) ); }
            bool expectedResult() { return true; }
        };
        
        /** $or passed 'false', 'false'. */
        class FalseFalse : public ExpectedResultBase {
            BSONObj spec() { return BSON( "$or" << BSON_ARRAY( false << false ) ); }
            bool expectedResult() { return false; }
        };
        
        /** $or passed 'false', 'false', 'false'. */
        class FalseFalseFalse : public ExpectedResultBase {
            BSONObj spec() { return BSON( "$or" << BSON_ARRAY( false << false << false ) ); }
            bool expectedResult() { return false; }
        };
        
        /** $or passed 'false', 'false', 'true'. */
        class FalseFalseTrue : public ExpectedResultBase {
            BSONObj spec() { return BSON( "$or" << BSON_ARRAY( false << false << true ) ); }
            bool expectedResult() { return true; }
        };
        
        /** $or passed '0', '1'. */
        class ZeroOne : public ExpectedResultBase {
            BSONObj spec() { return BSON( "$or" << BSON_ARRAY( 0 << 1 ) ); }
            bool expectedResult() { return true; }
        };
        
        /** $or passed '0', 'false'. */
        class ZeroFalse : public ExpectedResultBase {
            BSONObj spec() { return BSON( "$or" << BSON_ARRAY( 0 << false ) ); }
            bool expectedResult() { return false; }
        };
        
        /** $or passed a field path. */
        class FieldPath : public ExpectedResultBase {
            BSONObj spec() { return BSON( "$or" << BSON_ARRAY( "$a" ) ); }
            bool expectedResult() { return true; }
        };
        
        /** A constant expression is optimized to a constant. */
        class OptimizeConstantExpression : public OptimizeBase {
            BSONObj spec() { return BSON( "$or" << BSON_ARRAY( 1 ) ); }
            BSONObj expectedOptimized() { return BSON( "$const" << true ); }
        };
        
        /** A non constant expression is not optimized. */
        class NonConstant : public NoOptimizeBase {
            BSONObj spec() { return BSON( "$or" << BSON_ARRAY( "$a" ) ); }            
        };
        
        /** An expression beginning with a single constant is optimized. */
        class ConstantNonConstantTrue : public OptimizeBase {
            BSONObj spec() { return BSON( "$or" << BSON_ARRAY( 1 << "$a" ) ); }
            BSONObj expectedOptimized() { return BSON( "$const" << true ); }
        };

        /** An expression beginning with a single constant is optimized. */
        class ConstantNonConstantFalse : public OptimizeBase {
            BSONObj spec() { return BSON( "$or" << BSON_ARRAY( 0 << "$a" ) ); }
            BSONObj expectedOptimized() { return BSON( "$and" << BSON_ARRAY("$a") ); }
            // note: using $and as serialization of ExpressionCoerceToBool rather than ExpressionAnd
        };
        
        /** An expression with a field path and '1'. */
        class NonConstantOne : public OptimizeBase {
            BSONObj spec() { return BSON( "$or" << BSON_ARRAY( "$a" << 1 ) ); }
            BSONObj expectedOptimized() { return BSON( "$const" << true ); }
        };
        
        /** An expression with a field path and '0'. */
        class NonConstantZero : public OptimizeBase {
            BSONObj spec() { return BSON( "$or" << BSON_ARRAY( "$a" << 0 ) ); }
            BSONObj expectedOptimized() { return BSON( "$and" << BSON_ARRAY( "$a" ) ); }
        };
        
        /** An expression with two field paths and '1'. */
        class NonConstantNonConstantOne : public OptimizeBase {
            BSONObj spec() { return BSON( "$or" << BSON_ARRAY( "$a" << "$b" << 1 ) ); }
            BSONObj expectedOptimized() { return BSON( "$const" << true ); }
        };
        
        /** An expression with two field paths and '0'. */
        class NonConstantNonConstantZero : public OptimizeBase {
            BSONObj spec() { return BSON( "$or" << BSON_ARRAY( "$a" << "$b" << 0 ) ); }
            BSONObj expectedOptimized() { return BSON( "$or" << BSON_ARRAY( "$a" << "$b" ) ); }
        };
        
        /** An expression with '0', '1', and a field path. */
        class ZeroOneNonConstant : public OptimizeBase {
            BSONObj spec() { return BSON( "$or" << BSON_ARRAY( 0 << 1 << "$a" ) ); }
            BSONObj expectedOptimized() { return BSON( "$const" << true ); }
        };
        
        /** An expression with '0', '0', and a field path. */
        class ZeroZeroNonConstant : public OptimizeBase {
            BSONObj spec() { return BSON( "$or" << BSON_ARRAY( 0 << 0 << "$a" ) ); }
            BSONObj expectedOptimized() { return BSON( "$and" << BSON_ARRAY( "$a" ) ); }            
        };
        
        /** Nested $or expressions. */
        class Nested : public OptimizeBase {
            BSONObj spec() {
                return BSON( "$or" <<
                             BSON_ARRAY( 0 << BSON( "$or" << BSON_ARRAY( 0 ) ) << "$a" << "$b" ) );
            }
            BSONObj expectedOptimized() { return BSON( "$or" << BSON_ARRAY( "$a" << "$b" ) ); }            
        };
        
        /** Nested $or expressions containing a nested value evaluating to false. */
        class NestedOne : public OptimizeBase {
            BSONObj spec() {
                return BSON( "$or" <<
                             BSON_ARRAY( 0 <<
                                         BSON( "$or" <<
                                               BSON_ARRAY( BSON( "$or" <<
                                                                 BSON_ARRAY( 1 ) ) ) ) <<
                                         "$a" << "$b" ) );
            }
            BSONObj expectedOptimized() { return BSON( "$const" << true ); }
        };
        
    } // namespace Or

    namespace Parse {

        namespace Object {

            class Base {
            public:
                virtual ~Base() {}
                void run() {
                    BSONObj specObject = BSON( "" << spec() );
                    BSONElement specElement = specObject.firstElement();
                    Expression::ObjectCtx context = objectCtx();
                    intrusive_ptr<Expression> expression =
                            Expression::parseObject( &specElement, &context );
                    ASSERT_EQUALS( expectedBson(), expressionToBson( expression ) );
                }
            protected:
                virtual BSONObj spec() = 0;
                virtual Expression::ObjectCtx objectCtx() {
                    return Expression::ObjectCtx( Expression::ObjectCtx::DOCUMENT_OK );
                }
                virtual BSONObj expectedBson() { return constify( spec() ); }
            };

            class ParseError {
            public:
                virtual ~ParseError() {}
                void run() {
                    BSONObj specObject = BSON( "" << spec() );
                    BSONElement specElement = specObject.firstElement();
                    Expression::ObjectCtx context = objectCtx();
                    ASSERT_THROWS( Expression::parseObject( &specElement, &context ),
                                   UserException );
                }
            protected:
                virtual BSONObj spec() = 0;
                virtual Expression::ObjectCtx objectCtx() {
                    return Expression::ObjectCtx( Expression::ObjectCtx::DOCUMENT_OK );
                }
            };

            /** The spec must be an object. */
            class NonObject {
            public:
                void run() {
                    BSONObj specObject = BSON( "" << 1 );
                    BSONElement specElement = specObject.firstElement();
                    Expression::ObjectCtx context =
                            Expression::ObjectCtx( Expression::ObjectCtx::DOCUMENT_OK );
                    ASSERT_THROWS( Expression::parseObject( &specElement, &context ),
                                   UserException );                    
                }
            };

            /** Empty object. */
            class Empty : public Base {
                BSONObj spec() { return BSONObj(); }
            };

            /** Operator spec object. */
            class Operator : public Base {
                BSONObj spec() { return BSON( "$and" << BSONArray() ); }      
            };

            /** Invalid operator not allowed. */
            class InvalidOperator : public ParseError {
                BSONObj spec() { return BSON( "$invalid" << 1 ); }
            };            

            /** Two operators not allowed. */
            class TwoOperators : public ParseError {
                BSONObj spec() { return BSON( "$and" << BSONArray() << "$or" << BSONArray() ); }      
            };
            
            /** An operator must be the first and only field. */
            class OperatorLaterField : public ParseError {
                BSONObj spec() {
                    return BSON( "a" << BSON( "$and" << BSONArray() ) << "$or" << BSONArray() );
                }
            };
            
            /** An operator must be the first and only field. */
            class OperatorAndOtherField : public ParseError {
                BSONObj spec() {
                    return BSON( "$and" << BSONArray() << "a" << BSON( "$or" << BSONArray() ) );
                }
            };

            /** Operators not allowed at the top level of a projection. */
            class OperatorTopLevel : public ParseError {
                BSONObj spec() { return BSON( "$and" << BSONArray() ); }      
                Expression::ObjectCtx objectCtx() {
                    return Expression::ObjectCtx( Expression::ObjectCtx::DOCUMENT_OK |
                                                  Expression::ObjectCtx::TOP_LEVEL );
                }                
            };

            /** Dotted fields are not generally allowed. */
            class Dotted : public ParseError {
                BSONObj spec() { return BSON( "a.b" << BSON( "$and" << BSONArray() ) ); }                      
            };
            
            /** Dotted fields are allowed at the top level. */
            class DottedTopLevel : public Base {
                BSONObj spec() { return BSON( "a.b" << BSON( "$and" << BSONArray() ) ); }                      
                Expression::ObjectCtx objectCtx() {
                    return Expression::ObjectCtx( Expression::ObjectCtx::DOCUMENT_OK |
                                                  Expression::ObjectCtx::TOP_LEVEL );
                }
                BSONObj expectedBson() {
                    return BSON( "a" << BSON( "b" << BSON( "$and" << BSONArray() ) ) );
                }
            };
            
            /** Nested spec. */
            class Nested : public Base {
                BSONObj spec() { return BSON( "a" << BSON( "$and" << BSONArray() ) ); }
            };
            
            /** Parse error in nested document. */
            class NestedParseError : public ParseError {
                BSONObj spec() {
                    return BSON( "a" << BSON( "$and" << BSONArray() << "$or" << BSONArray() ) );
                }
            };

            /** FieldPath expression. */
            class FieldPath : public Base {
                BSONObj spec() { return BSON( "a" << "$field" ); }
            };

            /** Invalid FieldPath expression. */
            class InvalidFieldPath : public ParseError {
                BSONObj spec() { return BSON( "a" << "$field." ); }
            };

            /** Non FieldPath string. */
            class NonFieldPathString : public ParseError {
                BSONObj spec() { return BSON( "a" << "foo" ); }
            };

            /** Inclusion spec not allowed. */
            class DisallowedInclusion : public ParseError {
                BSONObj spec() { return BSON( "a" << 1 ); }
            };

            class InclusionBase : public Base {
                Expression::ObjectCtx objectCtx() {
                    return Expression::ObjectCtx( Expression::ObjectCtx::DOCUMENT_OK |
                                                  Expression::ObjectCtx::INCLUSION_OK );
                }
                BSONObj expectedBson() { return BSON( "a" << true ); }
            };

            /** Inclusion with bool type. */
            class InclusionBool : public InclusionBase {
                BSONObj spec() { return BSON( "a" << true ); }
            };

            /** Inclusion with double type. */
            class InclusionDouble : public InclusionBase {
                BSONObj spec() { return BSON( "a" << 1.0 ); }
            };

            /** Inclusion with int type. */
            class InclusionInt : public InclusionBase {
                BSONObj spec() { return BSON( "a" << 1 ); }
            };
            
            /** Inclusion with long type. */
            class InclusionLong : public InclusionBase {
                BSONObj spec() { return BSON( "a" << 1LL ); }
            };

            /** Inclusion of a nested field. */
            class NestedInclusion : public InclusionBase {
                BSONObj spec() { return BSON( "a" << BSON( "b" << true ) ); }                
                BSONObj expectedBson() { return spec(); }
            };

            /** Exclude _id. */
            class ExcludeId : public Base {
                BSONObj spec() { return BSON( "_id" << 0 ); }
                Expression::ObjectCtx objectCtx() {
                    return Expression::ObjectCtx( Expression::ObjectCtx::DOCUMENT_OK |
                                                  Expression::ObjectCtx::TOP_LEVEL );
                }
                BSONObj expectedBson() { return BSON( "_id" << false ); }
            };

            /** Excluding non _id field not allowed. */
            class ExcludeNonId : public ParseError {
                BSONObj spec() { return BSON( "a" << 0 ); }                
            };
            
            /** Excluding _id not top level. */
            class ExcludeIdNotTopLevel : public ParseError {
                BSONObj spec() { return BSON( "_id" << 0 ); }                
            };

            /** Invalid value type. */
            class InvalidType : public ParseError {
                BSONObj spec() { return BSON( "a" << BSONNULL ); }
            };

        } // namespace Object

        namespace Expression {

            using mongo::Expression;
            
            class Base {
            public:
                virtual ~Base() {}
                void run() {
                    BSONObj specObject = spec();
                    BSONElement specElement = specObject.firstElement();
                    intrusive_ptr<Expression> expression = Expression::parseExpression(specElement);
                    ASSERT_EQUALS( constify( expectedBson() ), expressionToBson( expression ) );
                }
            protected:
                virtual BSONObj spec() = 0;
                virtual BSONObj expectedBson() { return constify( spec() ); }
            };
            
            class ParseError {
            public:
                virtual ~ParseError() {}
                void run() {
                    BSONObj specObject = spec();
                    BSONElement specElement = specObject.firstElement();
                    ASSERT_THROWS(Expression::parseExpression(specElement), UserException);
                }
            protected:
                virtual BSONObj spec() = 0;
            };

            /** A constant expression. */
            class Const : public Base {
                BSONObj spec() { return BSON( "$const" << 5 ); }
            };

            /** An expression with an invalid name. */
            class InvalidName : public ParseError {
                BSONObj spec() { return BSON( "$invalid" << 1 ); }
            };

            /** An expression requiring an array that is not provided with an array. */
            class RequiredArrayMissing : public ParseError {
                BSONObj spec() { return BSON( "$strcasecmp" << "foo" ); }
            };

            /** An expression with the wrong number of operands. */
            class IncorrectOperandCount : public ParseError {
                BSONObj spec() { return BSON( "$strcasecmp" << BSON_ARRAY( "foo" ) ); }                
            };

            /** An expression with the correct number of operands. */
            class CorrectOperandCount : public Base {
                BSONObj spec() { return BSON( "$strcasecmp" << BSON_ARRAY( "foo" << "FOO" ) ); }
            };
            
            /** An variable argument expression with zero operands. */
            class ZeroOperands : public Base {
                BSONObj spec() { return BSON( "$and" << BSONArray() ); }
            };
            
            /** An variable argument expression with one operand. */
            class OneOperand : public Base {
                BSONObj spec() { return BSON( "$and" << BSON_ARRAY( 1 ) ); }
            };
            
            /** An variable argument expression with two operands. */
            class TwoOperands : public Base {
                BSONObj spec() { return BSON( "$and" << BSON_ARRAY( 1 << 2 ) ); }
            };

            /** An variable argument expression with a singleton operand. */
            class SingletonOperandVariable : public Base {
                BSONObj spec() { return BSON( "$and" << 1 ); }
                BSONObj expectedBson() { return BSON( "$and" << BSON_ARRAY( 1 ) ); }
            };

            /** An fixed argument expression with a singleton operand. */
            class SingletonOperandFixed : public Base {
                BSONObj spec() { return BSON( "$not" << 1 ); }
                BSONObj expectedBson() { return BSON( "$not" << BSON_ARRAY( 1 ) ); }
            };

            /** An object can be provided as a singleton argument. */
            class ObjectSingleton : public Base {
                BSONObj spec() { return BSON( "$and" << BSON( "$const" << 1 ) ); }
                BSONObj expectedBson() { return BSON("$and" << BSON_ARRAY(BSON("$const" << 1))); }
            };
            
            /** An object can be provided as an array agrument. */
            class ObjectOperand : public Base {
                BSONObj spec() { return BSON( "$and" << BSON_ARRAY( BSON( "$const" << 1 ) ) ); }
                BSONObj expectedBson() { return BSON( "$and" << BSON_ARRAY( 1 ) ); }
            };
            
        } // namespace Expression

        namespace Operand {

            class Base {
            public:
                virtual ~Base() {}
                void run() {
                    BSONObj specObject = spec();
                    BSONElement specElement = specObject.firstElement();
                    intrusive_ptr<mongo::Expression> expression =
                            mongo::Expression::parseOperand( specElement );
                    ASSERT_EQUALS( expectedBson(), expressionToBson( expression ) );
                }
            protected:
                virtual BSONObj spec() = 0;
                virtual BSONObj expectedBson() { return constify( spec() ); }
            };

            class ParseError {
            public:
                virtual ~ParseError() {}
                void run() {
                    BSONObj specObject = spec();
                    BSONElement specElement = specObject.firstElement();
                    ASSERT_THROWS( mongo::Expression::parseOperand( specElement ), UserException );
                }
            protected:
                virtual BSONObj spec() = 0;
            };

            /** A field path operand. */
            class FieldPath {
            public:
                void run() {
                    BSONObj specObject = BSON( "" << "$field" );
                    BSONElement specElement = specObject.firstElement();
                    intrusive_ptr<mongo::Expression> expression =
                            mongo::Expression::parseOperand( specElement );
                    ASSERT_EQUALS(specObject, BSON("" << expression->serialize()));
                }
            };

            /** A string constant (not field path) operand. */
            class NonFieldPathString : public Base {
                BSONObj spec() { return BSON( "" << "foo" ); }
                BSONObj expectedBson() { return BSON( "$const" << "foo" ); }
            };
            
            /** An object operand. */
            class Object : public Base {
                BSONObj spec() { return BSON( "" << BSON( "$and" << BSONArray() ) ); }
                BSONObj expectedBson() { return BSON( "$and" << BSONArray() ); }
            };

            /** An inclusion operand. */
            class InclusionObject : public ParseError {
                BSONObj spec() { return BSON( "" << BSON( "a" << 1 ) ); }
            };
            
            /** A constant operand. */
            class Constant : public Base {
                BSONObj spec() { return BSON( "" << 5 ); }
                BSONObj expectedBson() { return BSON( "$const" << 5 ); }
            };
            
        } // namespace Operand

    } // namespace Parse

    namespace Set {
        Value sortSet(Value set) {
            if (set.nullish()) {
                return Value(BSONNULL);
            }
            vector<Value> sortedSet = set.getArray();
            std::sort(sortedSet.begin(), sortedSet.end());
            return Value(sortedSet);
        }

        class ExpectedResultBase {
        public:
            virtual ~ExpectedResultBase() {}
            void run() {
                const Document spec = getSpec();
                const Value args = spec["input"];
                if (!spec["expected"].missing()) {
                    FieldIterator fields(spec["expected"].getDocument());
                    while (fields.more()) {
                        const Document::FieldPair field(fields.next());
                        const Value expected = field.second;
                        const BSONObj obj = BSON(field.first << args);
                        const intrusive_ptr<Expression> expr =
                                Expression::parseExpression(obj.firstElement());
                        Value result = expr->evaluate(Document());
                        if (result.getType() == Array) {
                            result = sortSet(result);
                        }
                        if (result != expected) {
                            string errMsg = str::stream()
                                << "for expression " << field.first.toString()
                                << " with argument " << args.toString()
                                << " full tree: " << expr->serialize().toString()
                                << " expected: " << expected.toString()
                                << " but got: " << result.toString();
                            FAIL(errMsg);
                        }
                        //TODO test optimize here
                    }
                }
                if (!spec["error"].missing()) {
                    const vector<Value>& asserters = spec["error"].getArray();
                    size_t n = asserters.size();
                    for (size_t i = 0; i < n; i++) {
                        const BSONObj obj = BSON(asserters[i].getString() << args);
                        ASSERT_THROWS({
                            // NOTE: parse and evaluatation failures are treated the same
                            const intrusive_ptr<Expression> expr =
                                    Expression::parseExpression(obj.firstElement());
                            expr->evaluate(Document());
                        }, UserException);
                    }
                }
            }
        private:
            virtual Document getSpec() = 0;
        };

        class Same : public ExpectedResultBase {
            Document getSpec() {
                return DOC("input" << DOC_ARRAY( DOC_ARRAY(1 << 2)
                                              << DOC_ARRAY(1 << 2) )
                        << "expected" << DOC("$setIsSubset" << true
                                          << "$setEquals" << true
                                          << "$setIntersection" << DOC_ARRAY(1 << 2)
                                          << "$setUnion" << DOC_ARRAY(1 << 2)
                                          << "$setDifference" << vector<Value>() )
                       );

            }
        };

        class Redundant : public ExpectedResultBase {
            Document getSpec() {
                return DOC("input" << DOC_ARRAY( DOC_ARRAY(1 << 2)
                                              << DOC_ARRAY(1 << 2 << 2) )
                        << "expected" << DOC("$setIsSubset" << true
                                          << "$setEquals" << true
                                          << "$setIntersection" << DOC_ARRAY(1 << 2)
                                          << "$setUnion" << DOC_ARRAY(1 << 2)
                                          << "$setDifference" << vector<Value>() )
                       );

            }
        };

        class DoubleRedundant : public ExpectedResultBase {
            Document getSpec() {
                return DOC("input" << DOC_ARRAY( DOC_ARRAY(1 << 1 << 2)
                                              << DOC_ARRAY(1 << 2 << 2) )
                        << "expected" << DOC("$setIsSubset" << true
                                          << "$setEquals" << true
                                          << "$setIntersection" << DOC_ARRAY(1 << 2)
                                          << "$setUnion" << DOC_ARRAY(1 << 2)
                                          << "$setDifference" << vector<Value>() )
                       );

            }
        };

        class Super : public ExpectedResultBase {
            Document getSpec() {
                return DOC("input" << DOC_ARRAY( DOC_ARRAY(1 << 2)
                                              << DOC_ARRAY(1) )
                        << "expected" << DOC("$setIsSubset" << false
                                          << "$setEquals" << false
                                          << "$setIntersection" << DOC_ARRAY(1)
                                          << "$setUnion" << DOC_ARRAY(1 << 2)
                                          << "$setDifference" << DOC_ARRAY(2) )
                       );

            }
        };

        class SuperWithRedundant : public ExpectedResultBase {
            Document getSpec() {
                return DOC("input" << DOC_ARRAY( DOC_ARRAY(1 << 2 << 2)
                                              << DOC_ARRAY(1) )
                        << "expected" << DOC("$setIsSubset" << false
                                          << "$setEquals" << false
                                          << "$setIntersection" << DOC_ARRAY(1)
                                          << "$setUnion" << DOC_ARRAY(1 << 2)
                                          << "$setDifference" << DOC_ARRAY(2) )
                       );

            }
        };

        class Sub : public ExpectedResultBase {
            Document getSpec() {
                return DOC("input" << DOC_ARRAY( DOC_ARRAY(1)
                                              << DOC_ARRAY(1 << 2) )
                        << "expected" << DOC("$setIsSubset" << true
                                          << "$setEquals" << false
                                          << "$setIntersection" << DOC_ARRAY(1)
                                          << "$setUnion" << DOC_ARRAY(1 << 2)
                                          << "$setDifference" << vector<Value>() )
                       );

            }
        };

        class SameBackwards : public ExpectedResultBase {
            Document getSpec() {
                return DOC("input" << DOC_ARRAY( DOC_ARRAY(1 << 2)
                                              << DOC_ARRAY(2 << 1) )
                        << "expected" << DOC("$setIsSubset" << true
                                          << "$setEquals" << true
                                          << "$setIntersection" << DOC_ARRAY(1 << 2)
                                          << "$setUnion" << DOC_ARRAY(1 << 2)
                                          << "$setDifference" << vector<Value>() )
                       );

            }
        };

        class NoOverlap : public ExpectedResultBase {
            Document getSpec() {
                return DOC("input" << DOC_ARRAY( DOC_ARRAY(1 << 2)
                                              << DOC_ARRAY(8 << 4) )
                        << "expected" << DOC("$setIsSubset" << false
                                          << "$setEquals" << false
                                          << "$setIntersection" << vector<Value>()
                                          << "$setUnion" << DOC_ARRAY(1 << 2 << 4 << 8)
                                          << "$setDifference" << DOC_ARRAY(1 << 2))
                       );

            }
        };

        class Overlap : public ExpectedResultBase {
            Document getSpec() {
                return DOC("input" << DOC_ARRAY( DOC_ARRAY(1 << 2)
                                              << DOC_ARRAY(8 << 2 << 4) )
                        << "expected" << DOC("$setIsSubset" << false
                                          << "$setEquals" << false
                                          << "$setIntersection" << DOC_ARRAY(2)
                                          << "$setUnion" << DOC_ARRAY(1 << 2 << 4 << 8)
                                          << "$setDifference" << DOC_ARRAY(1))
                       );

            }
        };

        class LastNull : public ExpectedResultBase {
            Document getSpec() {
                return DOC("input" << DOC_ARRAY( DOC_ARRAY(1 << 2)
                                              << Value(BSONNULL) )
                        << "expected" << DOC("$setIntersection" << BSONNULL
                                          << "$setUnion" << BSONNULL
                                          << "$setDifference" << BSONNULL )
                        << "error" << DOC_ARRAY("$setEquals"
                                             << "$setIsSubset")
                       );

            }
        };

        class FirstNull : public ExpectedResultBase {
            Document getSpec() {
                return DOC("input" << DOC_ARRAY( Value(BSONNULL)
                                              << DOC_ARRAY(1 << 2) )
                        << "expected" << DOC("$setIntersection" << BSONNULL
                                          << "$setUnion" << BSONNULL
                                          << "$setDifference" << BSONNULL )
                        << "error" << DOC_ARRAY("$setEquals"
                                             << "$setIsSubset")
                       );

            }
        };

        class NoArg : public ExpectedResultBase {
            Document getSpec() {
                return DOC("input" << vector<Value>()
                        << "expected" << DOC("$setIntersection" << vector<Value>()
                                          << "$setUnion" << vector<Value>() )
                        << "error" << DOC_ARRAY("$setEquals"
                                             << "$setIsSubset"
                                             << "$setDifference")
                       );

            }
        };

        class OneArg : public ExpectedResultBase {
            Document getSpec() {
                return DOC("input" << DOC_ARRAY( DOC_ARRAY(1 << 2) )
                        << "expected" << DOC("$setIntersection" << DOC_ARRAY(1 << 2)
                                          << "$setUnion" << DOC_ARRAY(1 << 2) )
                        << "error" << DOC_ARRAY("$setEquals"
                                             << "$setIsSubset"
                                             << "$setDifference")
                       );

            }
        };

        class EmptyArg : public ExpectedResultBase {
            Document getSpec() {
                return DOC("input" << DOC_ARRAY( vector<Value>() )
                        << "expected" << DOC("$setIntersection" << vector<Value>()
                                          << "$setUnion" << vector<Value>() )
                        << "error" << DOC_ARRAY("$setEquals"
                                             << "$setIsSubset"
                                             << "$setDifference")
                       );

            }
        };

        class LeftArgEmpty : public ExpectedResultBase {
            Document getSpec() {
                return DOC("input" << DOC_ARRAY( vector<Value>()
                                              << DOC_ARRAY(1 << 2) )
                        << "expected" << DOC("$setIntersection" << vector<Value>()
                                          << "$setUnion" << DOC_ARRAY(1 << 2)
                                          << "$setIsSubset" << true
                                          << "$setEquals" << false
                                          << "$setDifference" << vector<Value>() )
                       );

            }
        };

        class RightArgEmpty : public ExpectedResultBase {
            Document getSpec() {
                return DOC("input" << DOC_ARRAY( DOC_ARRAY(1 << 2)
                                              << vector<Value>() )
                        << "expected" << DOC("$setIntersection" << vector<Value>()
                                          << "$setUnion" << DOC_ARRAY(1 << 2)
                                          << "$setIsSubset" << false
                                          << "$setEquals" << false
                                          << "$setDifference" << DOC_ARRAY(1 << 2) )
                       );

            }
        };

        class ManyArgs : public ExpectedResultBase {
            Document getSpec() {
                return DOC("input" << DOC_ARRAY( DOC_ARRAY(8 << 3)
                                              << DOC_ARRAY("asdf" << "foo")
                                              << DOC_ARRAY(80.3 << 34)
                                              << vector<Value>()
                                              << DOC_ARRAY(80.3 << "foo" << 11 << "yay") )
                        << "expected" << DOC("$setIntersection" << vector<Value>()
                                          << "$setEquals" << false
                                          << "$setUnion" << DOC_ARRAY(3
                                                                   << 8
                                                                   << 11
                                                                   << 34
                                                                   << 80.3
                                                                   << "asdf"
                                                                   << "foo"
                                                                   << "yay") )
                        << "error" << DOC_ARRAY("$setIsSubset"
                                             << "$setDifference")
                       );

            }
        };

        class ManyArgsEqual : public ExpectedResultBase {
            Document getSpec() {
                return DOC("input" << DOC_ARRAY( DOC_ARRAY(1 << 2 << 4)
                                              << DOC_ARRAY(1 << 2 << 2 << 4)
                                              << DOC_ARRAY(4 << 1 << 2)
                                              << DOC_ARRAY(2 << 1 << 1 << 4) )
                        << "expected" << DOC("$setIntersection" << DOC_ARRAY(1 << 2 << 4)
                                          << "$setEquals" << true
                                          << "$setUnion" << DOC_ARRAY(1 << 2 << 4) )
                        << "error" << DOC_ARRAY("$setIsSubset"
                                             << "$setDifference")
                       );

            }
        };
    } // namespace Set

    namespace Strcasecmp {

        class ExpectedResultBase {
        public:
            virtual ~ExpectedResultBase() {
            }
            void run() {
                assertResult( expectedResult(), spec() );
                assertResult( -expectedResult(), reverseSpec() );
            }
        protected:
            virtual string a() = 0;
            virtual string b() = 0;
            virtual int expectedResult() = 0;
        private:
            BSONObj spec() { return BSON( "$strcasecmp" << BSON_ARRAY( a() << b() ) ); }
            BSONObj reverseSpec() { return BSON( "$strcasecmp" << BSON_ARRAY( b() << a() ) ); }
            void assertResult( int expectedResult, const BSONObj& spec ) {
                BSONObj specObj = BSON( "" << spec );
                BSONElement specElement = specObj.firstElement();
                intrusive_ptr<Expression> expression = Expression::parseOperand( specElement );
                ASSERT_EQUALS( constify( spec ), expressionToBson( expression ) );
                ASSERT_EQUALS( BSON( "" << expectedResult ),
                               toBson( expression->evaluate( Document() ) ) );                
            }
        };

        class NullBegin : public ExpectedResultBase {
            string a() { return string( "\0ab", 3 ); }
            string b() { return string( "\0AB", 3 ); }
            int expectedResult() { return 0; }            
        };

        class NullEnd : public ExpectedResultBase {
            string a() { return string( "ab\0", 3 ); }
            string b() { return string( "aB\0", 3 ); }
            int expectedResult() { return 0; }            
        };
        
        class NullMiddleLt : public ExpectedResultBase {
            string a() { return string( "a\0a", 3 ); }
            string b() { return string( "a\0B", 3 ); }
            int expectedResult() { return -1; }            
        };
        
        class NullMiddleEq : public ExpectedResultBase {
            string a() { return string( "a\0b", 3 ); }
            string b() { return string( "a\0B", 3 ); }
            int expectedResult() { return 0; }            
        };
        
        class NullMiddleGt : public ExpectedResultBase {
            string a() { return string( "a\0c", 3 ); }
            string b() { return string( "a\0B", 3 ); }
            int expectedResult() { return 1; }            
        };
        
    } // namespace Strcasecmp

    namespace Substr {

        class ExpectedResultBase {
        public:
            virtual ~ExpectedResultBase() {
            }
            void run() {
                BSONObj specObj = BSON( "" << spec() );
                BSONElement specElement = specObj.firstElement();
                intrusive_ptr<Expression> expression = Expression::parseOperand( specElement );
                ASSERT_EQUALS( constify( spec() ), expressionToBson( expression ) );
                ASSERT_EQUALS( BSON( "" << expectedResult() ),
                               toBson( expression->evaluate( Document() ) ) );
            }
        protected:
            virtual string str() = 0;
            virtual int offset() = 0;
            virtual int length() = 0;
            virtual string expectedResult() = 0;
        private:
            BSONObj spec() {
                return BSON( "$substr" << BSON_ARRAY( str() << offset() << length() ) );
            }
        };

        /** Retrieve a full string containing a null character. */
        class FullNull : public ExpectedResultBase {
            string str() { return string( "a\0b", 3 ); }
            int offset() { return 0; }
            int length() { return 3; }
            string expectedResult() { return str(); }
        };

        /** Retrieve a substring beginning with a null character. */
        class BeginAtNull : public ExpectedResultBase {
            string str() { return string( "a\0b", 3 ); }
            int offset() { return 1; }
            int length() { return 2; }
            string expectedResult() { return string( "\0b", 2 ); }
        };

        /** Retrieve a substring ending with a null character. */
        class EndAtNull : public ExpectedResultBase {
            string str() { return string( "a\0b", 3 ); }
            int offset() { return 0; }
            int length() { return 2; }
            string expectedResult() { return string( "a\0", 2 ); }
        };

        /** Drop a beginning null character. */
        class DropBeginningNull : public ExpectedResultBase {
            string str() { return string( "\0b", 2 ); }
            int offset() { return 1; }
            int length() { return 1; }
            string expectedResult() { return "b"; }
        };
        
        /** Drop an ending null character. */
        class DropEndingNull : public ExpectedResultBase {
            string str() { return string( "a\0", 2 ); }
            int offset() { return 0; }
            int length() { return 1; }
            string expectedResult() { return "a"; }
        };
        
    } // namespace Substr

    namespace ToLower {

        class ExpectedResultBase {
        public:
            virtual ~ExpectedResultBase() {
            }
            void run() {
                BSONObj specObj = BSON( "" << spec() );
                BSONElement specElement = specObj.firstElement();
                intrusive_ptr<Expression> expression = Expression::parseOperand( specElement );
                ASSERT_EQUALS( constify( spec() ), expressionToBson( expression ) );
                ASSERT_EQUALS( BSON( "" << expectedResult() ),
                               toBson( expression->evaluate( Document() ) ) );
            }
        protected:
            virtual string str() = 0;
            virtual string expectedResult() = 0;
        private:
            BSONObj spec() {
                return BSON( "$toLower" << BSON_ARRAY( str() ) );
            }
        };

        /** String beginning with a null character. */
        class NullBegin : public ExpectedResultBase {
            string str() { return string( "\0aB", 3 ); }
            string expectedResult() { return string( "\0ab", 3 ); }
        };

        /** String containing a null character. */
        class NullMiddle : public ExpectedResultBase {
            string str() { return string( "a\0B", 3 ); }
            string expectedResult() { return string( "a\0b", 3 ); }
        };
        
        /** String ending with a null character. */
        class NullEnd : public ExpectedResultBase {
            string str() { return string( "aB\0", 3 ); }
            string expectedResult() { return string( "ab\0", 3 ); }
        };
        
    } // namespace ToLower

    namespace ToUpper {

        class ExpectedResultBase {
        public:
            virtual ~ExpectedResultBase() {
            }
            void run() {
                BSONObj specObj = BSON( "" << spec() );
                BSONElement specElement = specObj.firstElement();
                intrusive_ptr<Expression> expression = Expression::parseOperand( specElement );
                ASSERT_EQUALS( constify( spec() ), expressionToBson( expression ) );
                ASSERT_EQUALS( BSON( "" << expectedResult() ),
                               toBson( expression->evaluate( Document() ) ) );
            }
        protected:
            virtual string str() = 0;
            virtual string expectedResult() = 0;
        private:
            BSONObj spec() {
                return BSON( "$toUpper" << BSON_ARRAY( str() ) );
            }
        };

        /** String beginning with a null character. */
        class NullBegin : public ExpectedResultBase {
            string str() { return string( "\0aB", 3 ); }
            string expectedResult() { return string( "\0AB", 3 ); }
        };

        /** String containing a null character. */
        class NullMiddle : public ExpectedResultBase {
            string str() { return string( "a\0B", 3 ); }
            string expectedResult() { return string( "A\0B", 3 ); }
        };
        
        /** String ending with a null character. */
        class NullEnd : public ExpectedResultBase {
            string str() { return string( "aB\0", 3 ); }
            string expectedResult() { return string( "AB\0", 3 ); }
        };
        
    } // namespace ToUpper

    namespace AllAnyElements {
        class ExpectedResultBase {
        public:
            virtual ~ExpectedResultBase() {}
            void run() {
                const Document spec = getSpec();
                const Value args = spec["input"];
                if (!spec["expected"].missing()) {
                    FieldIterator fields(spec["expected"].getDocument());
                    while (fields.more()) {
                        const Document::FieldPair field(fields.next());
                        const Value expected = field.second;
                        const BSONObj obj = BSON(field.first << args);
                        const intrusive_ptr<Expression> expr =
                                Expression::parseExpression(obj.firstElement());
                        const Value result = expr->evaluate(Document());
                        if (result != expected) {
                            string errMsg = str::stream()
                                                << "for expression " << field.first.toString()
                                                << " with argument " << args.toString()
                                                << " full tree: " << expr->serialize().toString()
                                                << " expected: " << expected.toString()
                                                << " but got: " << result.toString();
                            FAIL(errMsg);
                        }
                        //TODO test optimize here
                    }
                }
                if (!spec["error"].missing()) {
                    const vector<Value>& asserters = spec["error"].getArray();
                    size_t n = asserters.size();
                    for (size_t i = 0; i < n; i++) {
                        const BSONObj obj = BSON(asserters[i].getString() << args);
                        ASSERT_THROWS({
                            // NOTE: parse and evaluatation failures are treated the same
                            const intrusive_ptr<Expression> expr =
                                    Expression::parseExpression(obj.firstElement());
                            expr->evaluate(Document());
                        }, UserException);
                    }
                }
            }
        private:
            virtual Document getSpec() = 0;
        };

        class JustFalse : public ExpectedResultBase {
            Document getSpec() {
                return DOC("input" << DOC_ARRAY( DOC_ARRAY(false) )
                        << "expected" << DOC("$allElementsTrue" << false
                                          << "$anyElementTrue" << false) );
            }
        };

        class JustTrue : public ExpectedResultBase {
            Document getSpec() {
                return DOC("input" << DOC_ARRAY( DOC_ARRAY(true) )
                        << "expected" << DOC("$allElementsTrue" << true
                                          << "$anyElementTrue" << true) );
            }
        };

        class OneTrueOneFalse : public ExpectedResultBase {
            Document getSpec() {
                return DOC("input" << DOC_ARRAY( DOC_ARRAY(true << false) )
                        << "expected" << DOC("$allElementsTrue" << false
                                          << "$anyElementTrue" << true) );
            }
        };

        class Empty : public ExpectedResultBase {
            Document getSpec() {
                return DOC("input" << DOC_ARRAY( vector<Value>() )
                        << "expected" << DOC("$allElementsTrue" << true
                                          << "$anyElementTrue" << false) );
            }
        };

        class TrueViaInt : public ExpectedResultBase {
            Document getSpec() {
                return DOC("input" << DOC_ARRAY( DOC_ARRAY(1) )
                        << "expected" << DOC("$allElementsTrue" << true
                                          << "$anyElementTrue" << true) );
            }
        };

        class FalseViaInt : public ExpectedResultBase {
            Document getSpec() {
                return DOC("input" << DOC_ARRAY( DOC_ARRAY(0) )
                        << "expected" << DOC("$allElementsTrue" << false
                                          << "$anyElementTrue" << false) );
            }
        };

        class Null : public ExpectedResultBase {
            Document getSpec() {
                return DOC("input" << DOC_ARRAY(BSONNULL)
                        << "error" << DOC_ARRAY("$allElementsTrue"
                                             << "$anyElementTrue") );
            }
        };

    } // namespace AllAnyElements

    class All : public Suite {
    public:
        All() : Suite( "expression" ) {
        }
        void setupTests() {
            add<Add::NullDocument>();
            add<Add::NoOperands>();
            add<Add::Date>();
            add<Add::String>();
            add<Add::Bool>();
            add<Add::Int>();
            add<Add::Long>();
            add<Add::Double>();
            add<Add::Null>();
            add<Add::Undefined>();
            add<Add::IntInt>();
            add<Add::IntIntNoOverflow>();
            add<Add::IntLong>();
            add<Add::IntLongOverflow>();
            add<Add::IntDouble>();
            add<Add::IntDate>();
            add<Add::LongDouble>();
            add<Add::LongDoubleNoOverflow>();
            add<Add::IntNull>();
            add<Add::LongUndefined>();

            add<And::NoOperands>();
            add<And::True>();
            add<And::False>();
            add<And::TrueTrue>();
            add<And::TrueFalse>();
            add<And::FalseTrue>();
            add<And::FalseFalse>();
            add<And::TrueTrueTrue>();
            add<And::TrueTrueFalse>();
            add<And::TrueTrueFalse>();
            add<And::ZeroOne>();
            add<And::OneTwo>();
            add<And::FieldPath>();
            add<And::OptimizeConstantExpression>();
            add<And::NonConstant>();
            add<And::ConstantNonConstantTrue>();
            add<And::ConstantNonConstantFalse>();
            add<And::NonConstantOne>();
            add<And::NonConstantZero>();
            add<And::NonConstantNonConstantOne>();
            add<And::NonConstantNonConstantZero>();
            add<And::ZeroOneNonConstant>();
            add<And::OneOneNonConstant>();
            add<And::Nested>();
            add<And::NestedZero>();

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
            add<FieldPath::NestedBelowMissing>();
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

            add<FieldRange::EqLt>();
            add<FieldRange::EqEq>();
            add<FieldRange::EqGt>();
            add<FieldRange::LtLt>();
            add<FieldRange::LtEq>();
            add<FieldRange::LtGt>();
            add<FieldRange::LteLt>();
            add<FieldRange::LteEq>();
            add<FieldRange::LteGt>();
            add<FieldRange::GtLt>();
            add<FieldRange::GtEq>();
            add<FieldRange::GtGt>();
            add<FieldRange::GteLt>();
            add<FieldRange::GteEq>();
            add<FieldRange::GteGt>();
            add<FieldRange::Dependencies>();
            add<FieldRange::Multikey>();

            add<Nary::AddOperand>();
            add<Nary::Dependencies>();
            add<Nary::AddToBsonObj>();
            add<Nary::AddToBsonArray>();
            add<Nary::OptimizeOneOperand>();
            add<Nary::EvaluateAllConstantOperands>();
            add<Nary::StringConstant>();
            add<Nary::SingleConstant>();
            add<Nary::NoFactory>();
            add<Nary::FactoryOptimize>();
            add<Nary::FlattenOptimize>();
            add<Nary::FlattenThreeLayers>();

            add<Object::Empty>();
            add<Object::Include>();
            add<Object::MissingInclude>();
            add<Object::IncludeId>();
            add<Object::ExcludeId>();
            add<Object::SourceOrder>();
            add<Object::IncludeNested>();
            add<Object::IncludeTwoNested>();
            add<Object::IncludeTwoParentNested>();
            add<Object::IncludeMissingNested>();
            add<Object::IncludeNestedWithinNonObject>();
            add<Object::IncludeArrayNested>();
            add<Object::ExcludeNonRootId>();
            add<Object::Computed>();
            add<Object::ComputedReplacement>();
            add<Object::ComputedUndefined>();
            add<Object::ComputedUndefinedReplacement>();
            add<Object::ComputedNull>();
            add<Object::ComputedNested>();
            add<Object::ComputedFieldPath>();
            add<Object::ComputedNestedFieldPath>();
            add<Object::EmptyNewSubobject>();
            add<Object::NonEmptyNewSubobject>();
            add<Object::AdjacentNestedComputedFields>();
            add<Object::AdjacentDottedAndNestedComputedFields>();
            add<Object::AdjacentNestedAndDottedComputedFields>();
            add<Object::AdjacentDottedComputedFields>();            
            add<Object::AdjacentNestedOrdering>();
            add<Object::MultipleNestedFields>();
            add<Object::ConflictingExpressionFields>();
            add<Object::ConflictingInclusionExpressionFields>();
            add<Object::ConflictingExpressionInclusionFields>();
            add<Object::ConflictingObjectConstantExpressionFields>();
            add<Object::ConflictingConstantObjectExpressionFields>();
            add<Object::ConflictingNestedFields>();
            add<Object::ConflictingFieldAndSubfield>();
            add<Object::ConflictingFieldAndNestedField>();
            add<Object::ConflictingSubfieldAndField>();
            add<Object::ConflictingNestedFieldAndField>();
            add<Object::NonInclusionDependencies>();
            add<Object::InclusionDependencies>();
            add<Object::Optimize>();
            add<Object::AddToBsonObj>();
            add<Object::AddToBsonObjRequireExpression>();
            add<Object::AddToBsonArray>();
            add<Object::Evaluate>();

            add<Or::NoOperands>();
            add<Or::True>();
            add<Or::False>();
            add<Or::TrueTrue>();
            add<Or::TrueFalse>();
            add<Or::FalseTrue>();
            add<Or::FalseFalse>();
            add<Or::FalseFalseFalse>();
            add<Or::FalseFalseTrue>();
            add<Or::ZeroOne>();
            add<Or::ZeroFalse>();
            add<Or::FieldPath>();
            add<Or::OptimizeConstantExpression>();
            add<Or::NonConstant>();
            add<Or::ConstantNonConstantTrue>();
            add<Or::ConstantNonConstantFalse>();
            add<Or::NonConstantOne>();
            add<Or::NonConstantZero>();
            add<Or::NonConstantNonConstantOne>();
            add<Or::NonConstantNonConstantZero>();
            add<Or::ZeroOneNonConstant>();
            add<Or::ZeroZeroNonConstant>();
            add<Or::Nested>();
            add<Or::NestedOne>();

            add<Parse::Object::NonObject>();
            add<Parse::Object::Empty>();
            add<Parse::Object::Operator>();
            add<Parse::Object::InvalidOperator>();
            add<Parse::Object::TwoOperators>();
            add<Parse::Object::OperatorLaterField>();
            add<Parse::Object::OperatorAndOtherField>();
            add<Parse::Object::OperatorTopLevel>();
            add<Parse::Object::Dotted>();
            add<Parse::Object::DottedTopLevel>();
            add<Parse::Object::Nested>();
            add<Parse::Object::NestedParseError>();
            add<Parse::Object::FieldPath>();
            add<Parse::Object::InvalidFieldPath>();
            add<Parse::Object::NonFieldPathString>();
            add<Parse::Object::DisallowedInclusion>();
            add<Parse::Object::InclusionBool>();
            add<Parse::Object::InclusionDouble>();
            add<Parse::Object::InclusionInt>();
            add<Parse::Object::InclusionLong>();
            add<Parse::Object::NestedInclusion>();
            add<Parse::Object::ExcludeId>();
            add<Parse::Object::ExcludeNonId>();
            add<Parse::Object::ExcludeIdNotTopLevel>();
            add<Parse::Object::InvalidType>();
            add<Parse::Expression::Const>();
            add<Parse::Expression::InvalidName>();
            add<Parse::Expression::RequiredArrayMissing>();
            add<Parse::Expression::IncorrectOperandCount>();
            add<Parse::Expression::CorrectOperandCount>();
            add<Parse::Expression::ZeroOperands>();
            add<Parse::Expression::OneOperand>();
            add<Parse::Expression::TwoOperands>();
            add<Parse::Expression::SingletonOperandVariable>();
            add<Parse::Expression::SingletonOperandFixed>();
            add<Parse::Expression::ObjectSingleton>();
            add<Parse::Expression::ObjectOperand>();
            add<Parse::Operand::FieldPath>();
            add<Parse::Operand::NonFieldPathString>();
            add<Parse::Operand::Object>();
            add<Parse::Operand::InclusionObject>();
            add<Parse::Operand::Constant>();

            add<Strcasecmp::NullBegin>();
            add<Strcasecmp::NullEnd>();
            add<Strcasecmp::NullMiddleLt>();
            add<Strcasecmp::NullMiddleEq>();
            add<Strcasecmp::NullMiddleGt>();

            add<Substr::FullNull>();
            add<Substr::BeginAtNull>();
            add<Substr::EndAtNull>();
            add<Substr::DropBeginningNull>();
            add<Substr::DropEndingNull>();

            add<ToLower::NullBegin>();
            add<ToLower::NullMiddle>();
            add<ToLower::NullEnd>();

            add<ToUpper::NullBegin>();
            add<ToUpper::NullMiddle>();
            add<ToUpper::NullEnd>();

            add<Set::Same>();
            add<Set::Redundant>();
            add<Set::DoubleRedundant>();
            add<Set::Sub>();
            add<Set::Super>();
            add<Set::SameBackwards>();
            add<Set::NoOverlap>();
            add<Set::Overlap>();
            add<Set::FirstNull>();
            add<Set::LastNull>();
            add<Set::NoArg>();
            add<Set::OneArg>();
            add<Set::EmptyArg>();
            add<Set::LeftArgEmpty>();
            add<Set::RightArgEmpty>();
            add<Set::ManyArgs>();
            add<Set::ManyArgsEqual>();

            add<AllAnyElements::JustFalse>();
            add<AllAnyElements::JustTrue>();
            add<AllAnyElements::OneTrueOneFalse>();
            add<AllAnyElements::Empty>();
            add<AllAnyElements::TrueViaInt>();
            add<AllAnyElements::FalseViaInt>();
            add<AllAnyElements::Null>();
        }
    } myall;

} // namespace ExpressionTests
