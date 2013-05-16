// accumulatortests.cpp : Unit tests for Accumulator classes.

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

#include "mongo/db/pipeline/accumulator.h"

#include "mongo/db/interrupt_status_mongod.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/expression_context.h"

#include "dbtests.h"

namespace AccumulatorTests {

    class Base {
    public:
        Base() :
            _standalone( ExpressionContext::create( &InterruptStatusMongod::status ) ),
            _shard( ExpressionContext::create( &InterruptStatusMongod::status ) ),
            _router( ExpressionContext::create( &InterruptStatusMongod::status ) ) {
                _standalone->setInShard( false );
                _standalone->setDoingMerge( false );
                _shard->setInShard( true );
                _shard->setDoingMerge( false );
                _router->setInShard( false );
                _router->setDoingMerge( true );
        }
    protected:
        Document fromjson( const string& json ) {
            return frombson( mongo::fromjson( json ) );
        }
        Document frombson( const BSONObj& bson ) {
            BSONObj myBson = bson;
            return Document::createFromBsonObj( &myBson );
        }
        BSONObj fromDocument( const Document& document ) {
            BSONObjBuilder bob;
            document->toBson( &bob );
            return bob.obj();
        }
        BSONObj fromValue( const Value& value ) {
            BSONObjBuilder bob;
            value.addToBsonObj( &bob, "" );
            return bob.obj();
        }
        /** Check binary equality, ensuring use of the same numeric types. */
        void assertBinaryEqual( const BSONObj& expected, const BSONObj& actual ) const {
            ASSERT_EQUALS( expected, actual );
            ASSERT( expected.binaryEqual( actual ) );
        }
        void assertBsonRepresentation( const BSONObj& expected,
                                       const intrusive_ptr<Accumulator>& accumulator ) const {
            BSONObjBuilder bob;
            bob << "" << accumulator->serialize();
            assertBinaryEqual( expected, bob.obj().firstElement().Obj().getOwned() );
        }
        intrusive_ptr<ExpressionContext> standalone() const { return _standalone; }
        intrusive_ptr<ExpressionContext> shard() const { return _shard; }
        intrusive_ptr<ExpressionContext> router() const { return _router; }
    private:
        intrusive_ptr<ExpressionContext> _standalone;
        intrusive_ptr<ExpressionContext> _shard;
        intrusive_ptr<ExpressionContext> _router;        
    };

    namespace Avg {
        
        class Base : public AccumulatorTests::Base {
        public:
            virtual ~Base() {
            }
        protected:
            virtual intrusive_ptr<ExpressionContext> context() { return standalone(); }
            void createAccumulator() {
                _accumulator = AccumulatorAvg::create( context() );
                _accumulator->addOperand( ExpressionFieldPath::create( "d" ) );
                assertBsonRepresentation( BSON( "$avg" << "$d" ), _accumulator );
            }
            Accumulator *accumulator() { return _accumulator.get(); }
        private:
            intrusive_ptr<Accumulator> _accumulator;
        };
        
        /** No documents evaluated. */
        class None : public Base {
        public:
            void run() {
                createAccumulator();
                ASSERT_EQUALS( 0, accumulator()->getValue().getDouble() );
            }
        };
        
        /** One int value is converted to double. */
        class OneInt : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->evaluate( frombson( BSON( "d" << 3 ) ) );
                ASSERT_EQUALS( 3, accumulator()->getValue().getDouble() );
            }
        };
        
        /** One long value is converted to double. */
        class OneLong : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->evaluate( frombson( BSON( "d" << -4LL ) ) );
                ASSERT_EQUALS( -4, accumulator()->getValue().getDouble() );
            }
        };
        
        /** One double value. */
        class OneDouble : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->evaluate( frombson( BSON( "d" << 22.6 ) ) );
                ASSERT_EQUALS( 22.6, accumulator()->getValue().getDouble() );
            }
        };
        
        /** The average of two ints is an int, even if inexact. */
        class IntInt : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->evaluate( frombson( BSON( "d" << 10 ) ) );
                accumulator()->evaluate( frombson( BSON( "d" << 11 ) ) );
                ASSERT_EQUALS( 10.5, accumulator()->getValue().getDouble() );
            }
        };        
        
        /** The average of an int and a double is calculated as a double. */
        class IntDouble : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->evaluate( frombson( BSON( "d" << 10 ) ) );
                accumulator()->evaluate( frombson( BSON( "d" << 11.0 ) ) );
                ASSERT_EQUALS( 10.5, accumulator()->getValue().getDouble() );
            }
        };        
        
        /** Unlike $sum, two ints do not overflow in the 'total' portion of the average. */
        class IntIntNoOverflow : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->evaluate( frombson( BSON( "d" << numeric_limits<int>::max() ) ) );
                accumulator()->evaluate( frombson( BSON( "d" << numeric_limits<int>::max() ) ) );
                ASSERT_EQUALS( numeric_limits<int>::max(), accumulator()->getValue().getDouble() );
            }
        };        
        
        /** Two longs do overflow in the 'total' portion of the average. */
        class LongLongOverflow : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->evaluate
                        ( frombson( BSON( "d" << numeric_limits<long long>::max() ) ) );
                accumulator()->evaluate
                        ( frombson( BSON( "d" << numeric_limits<long long>::max() ) ) );
                ASSERT_EQUALS( ( (double)numeric_limits<long long>::max() +
                                 numeric_limits<long long>::max() ) / 2.0,
                               accumulator()->getValue().getDouble() );
            }
        };

        namespace Shard {

            class Base : public Avg::Base {
                virtual intrusive_ptr<ExpressionContext> context() { return shard(); }
            };

            class SingleOperandBase : public Base {
            public:
                void run() {
                    createAccumulator();
                    accumulator()->evaluate( frombson( operand() ) );
                    assertBinaryEqual( expectedResult(),
                                       fromDocument( accumulator()->getValue().getDocument() ) );
                }
            protected:
                virtual BSONObj operand() = 0;
                virtual BSONObj expectedResult() = 0;
            };

            /** Shard result for one integer. */
            class Int : public SingleOperandBase {
                BSONObj operand() { return BSON( "d" << 3 ); }
                BSONObj expectedResult() { return BSON( "subTotal" << 3.0 << "count" << 1LL ); }
            };
            
            /** Shard result for one long. */
            class Long : public SingleOperandBase {
                BSONObj operand() { return BSON( "d" << 5LL ); }
                BSONObj expectedResult() { return BSON( "subTotal" << 5.0 << "count" << 1LL ); }
            };
            
            /** Shard result for one double. */
            class Double : public SingleOperandBase {
                BSONObj operand() { return BSON( "d" << 116.0 ); }
                BSONObj expectedResult() { return BSON( "subTotal" << 116.0 << "count" << 1LL ); }
            };

            class TwoOperandBase : public Base {
            public:
                void run() {
                    checkAvg( operand1(), operand2() );
                    checkAvg( operand2(), operand1() );
                }
            protected:
                virtual BSONObj operand1() = 0;
                virtual BSONObj operand2() = 0;
                virtual BSONObj expectedResult() = 0;
            private:
                void checkAvg( const BSONObj& a, const BSONObj& b ) {
                    createAccumulator();
                    accumulator()->evaluate( frombson( a ) );
                    accumulator()->evaluate( frombson( b ) );
                    assertBinaryEqual( expectedResult(),
                                       fromDocument( accumulator()->getValue().getDocument() ) );
                }
            };

            /** Shard two ints overflow. */
            class IntIntOverflow : public TwoOperandBase {
                BSONObj operand1() { return BSON( "d" << numeric_limits<int>::max() ); }
                BSONObj operand2() { return BSON( "d" << 3 ); }
                BSONObj expectedResult() {
                    return BSON( "subTotal" << numeric_limits<int>::max() + 3.0 << "count" << 2LL );
                }
            };

            /** Shard avg an int and a long. */
            class IntLong : public TwoOperandBase {
                BSONObj operand1() { return BSON( "d" << 5 ); }
                BSONObj operand2() { return BSON( "d" << 3LL ); }
                BSONObj expectedResult() { return BSON( "subTotal" << 8.0 << "count" << 2LL ); }
            };
            
            /** Shard avg an int and a double. */
            class IntDouble : public TwoOperandBase {
                BSONObj operand1() { return BSON( "d" << 5 ); }
                BSONObj operand2() { return BSON( "d" << 6.2 ); }
                BSONObj expectedResult() { return BSON( "subTotal" << 11.2 << "count" << 2LL ); }
            };
            
            /** Shard avg a long and a double. */
            class LongDouble : public TwoOperandBase {
                BSONObj operand1() { return BSON( "d" << 5LL ); }
                BSONObj operand2() { return BSON( "d" << 1.0 ); }
                BSONObj expectedResult() { return BSON( "subTotal" << 6.0 << "count" << 2LL ); }
            };

            /** Shard avg an int, long, and double. */
            class IntLongDouble : public Base {
            public:
                void run() {
                    createAccumulator();
                    accumulator()->evaluate( frombson( BSON( "d" << 1 ) ) );
                    accumulator()->evaluate( frombson( BSON( "d" << 2LL ) ) );
                    accumulator()->evaluate( frombson( BSON( "d" << 4.0 ) ) );
                    assertBinaryEqual( BSON( "subTotal" << 7.0 << "count" << 3LL ),
                                       fromDocument( accumulator()->getValue().getDocument() ) );
                }
            };

        } // namespace Shard

        namespace Router {

            class Base : public Avg::Base {
                virtual intrusive_ptr<ExpressionContext> context() { return router(); }
            };

            /** Router result from one shard. */
            class OneShard : public Base {
            public:
                void run() {
                    createAccumulator();
                    accumulator()->evaluate
                            ( frombson
                             ( BSON( "d" << BSON( "subTotal" << 3.0 << "count" << 2LL ) ) ) );
                    assertBinaryEqual( BSON( "" << 3.0 / 2 ),
                                       fromValue( accumulator()->getValue() ) );
                }
            };
            
            /** Router result from two shards. */
            class TwoShards : public Base {
            public:
                void run() {
                    createAccumulator();
                    accumulator()->evaluate
                            ( frombson
                             ( BSON( "d" << BSON( "subTotal" << 6.0 << "count" << 1LL ) ) ) );
                    accumulator()->evaluate
                            ( frombson
                             ( BSON( "d" << BSON( "subTotal" << 5.0 << "count" << 2LL ) ) ) );
                    assertBinaryEqual( BSON( "" << 11.0 / 3 ),
                                       fromValue( accumulator()->getValue() ) );
                }
            };

        } // namespace Router
        
    } // namespace Avg

    namespace First {

        class Base : public AccumulatorTests::Base {
        protected:
            void createAccumulator() {
                _accumulator = AccumulatorFirst::create( standalone() );
                _accumulator->addOperand( ExpressionFieldPath::create( "a" ) );
                assertBsonRepresentation( BSON( "$first" << "$a" ), _accumulator );
            }
            Accumulator *accumulator() { return _accumulator.get(); }
        private:
            intrusive_ptr<Accumulator> _accumulator;
        };

        /** The accumulator evaluates no documents. */
        class None : public Base {
        public:
            void run() {
                createAccumulator();
                // The accumulator returns no value in this case.
                ASSERT( accumulator()->getValue().missing() );
            }
        };

        /* The accumulator evaluates one document and retains its value. */
        class One : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->evaluate( fromjson( "{a:5}" ) );
                ASSERT_EQUALS( 5, accumulator()->getValue().getInt() );
            }
        };
        
        /* The accumulator evaluates one document with the field missing, returns missing value. */
        class Missing : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->evaluate( fromjson( "{}" ) );
                ASSERT_EQUALS( EOO, accumulator()->getValue().getType() );
            }
        };
        
        /* The accumulator evaluates two documents and retains the value in the first. */
        class Two : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->evaluate( fromjson( "{a:5}" ) );
                accumulator()->evaluate( fromjson( "{a:7}" ) );
                ASSERT_EQUALS( 5, accumulator()->getValue().getInt() );
            }
        };
        
        /* The accumulator evaluates two documents and retains the missing value in the first. */
        class FirstMissing : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->evaluate( fromjson( "{}" ) );
                accumulator()->evaluate( fromjson( "{a:7}" ) );
                ASSERT_EQUALS( EOO, accumulator()->getValue().getType() );
            }
        };
        
    } // namespace First

    namespace Last {
        
        class Base : public AccumulatorTests::Base {
        protected:
            void createAccumulator() {
                _accumulator = AccumulatorLast::create( standalone() );
                _accumulator->addOperand( ExpressionFieldPath::create( "b" ) );
                assertBsonRepresentation( BSON( "$last" << "$b" ), _accumulator );
            }
            Accumulator *accumulator() { return _accumulator.get(); }
        private:
            intrusive_ptr<Accumulator> _accumulator;
        };
        
        /** The accumulator evaluates no documents. */
        class None : public Base {
        public:
            void run() {
                createAccumulator();
                // The accumulator returns no value in this case.
                ASSERT( accumulator()->getValue().missing() );
            }
        };
        
        /* The accumulator evaluates one document and retains its value. */
        class One : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->evaluate( fromjson( "{b:5}" ) );
                ASSERT_EQUALS( 5, accumulator()->getValue().getInt() );
            }
        };
        
        /* The accumulator evaluates one document with the field missing retains undefined. */
        class Missing : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->evaluate( fromjson( "{}" ) );
                ASSERT_EQUALS( EOO , accumulator()->getValue().getType() );
            }
        };
        
        /* The accumulator evaluates two documents and retains the value in the last. */
        class Two : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->evaluate( fromjson( "{b:5}" ) );
                accumulator()->evaluate( fromjson( "{b:7}" ) );
                ASSERT_EQUALS( 7, accumulator()->getValue().getInt() );
            }
        };
        
        /* The accumulator evaluates two documents and retains the undefined value in the last. */
        class LastMissing : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->evaluate( fromjson( "{b:7}" ) );
                accumulator()->evaluate( fromjson( "{}" ) );
                ASSERT_EQUALS( EOO , accumulator()->getValue().getType() );
            }
        };
        
    } // namespace Last
    
    namespace Min {
        
        class Base : public AccumulatorTests::Base {
        protected:
            void createAccumulator() {
                _accumulator = AccumulatorMinMax::createMin( standalone() );
                _accumulator->addOperand( ExpressionFieldPath::create( "c" ) );
                assertBsonRepresentation( BSON( "$min" << "$c" ), _accumulator );
            }
            Accumulator *accumulator() { return _accumulator.get(); }
        private:
            intrusive_ptr<Accumulator> _accumulator;
        };
        
        /** The accumulator evaluates no documents. */
        class None : public Base {
        public:
            void run() {
                createAccumulator();
                // The accumulator returns no value in this case.
                ASSERT( accumulator()->getValue().missing() );
            }
        };
        
        /* The accumulator evaluates one document and retains its value. */
        class One : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->evaluate( fromjson( "{c:5}" ) );
                ASSERT_EQUALS( 5, accumulator()->getValue().getInt() );
            }
        };
        
        /* The accumulator evaluates one document with the field missing retains undefined. */
        class Missing : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->evaluate( fromjson( "{}" ) );
                ASSERT_EQUALS( EOO , accumulator()->getValue().getType() );
            }
        };
        
        /* The accumulator evaluates two documents and retains the minimum value. */
        class Two : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->evaluate( fromjson( "{c:5}" ) );
                accumulator()->evaluate( fromjson( "{c:7}" ) );
                ASSERT_EQUALS( 5, accumulator()->getValue().getInt() );
            }
        };
        
        /* The accumulator evaluates two documents and retains the undefined value. */
        class LastMissing : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->evaluate( fromjson( "{c:7}" ) );
                accumulator()->evaluate( fromjson( "{}" ) );
                ASSERT_EQUALS( 7 , accumulator()->getValue().getInt() );
            }
        };
        
    } // namespace Min
    
    namespace Max {
        
        class Base : public AccumulatorTests::Base {
        protected:
            void createAccumulator() {
                _accumulator = AccumulatorMinMax::createMax( standalone() );
                _accumulator->addOperand( ExpressionFieldPath::create( "d" ) );
                assertBsonRepresentation( BSON( "$max" << "$d" ), _accumulator );
            }
            Accumulator *accumulator() { return _accumulator.get(); }
        private:
            intrusive_ptr<Accumulator> _accumulator;
        };
        
        /** The accumulator evaluates no documents. */
        class None : public Base {
        public:
            void run() {
                createAccumulator();
                // The accumulator returns no value in this case.
                ASSERT( accumulator()->getValue().missing() );
            }
        };
        
        /* The accumulator evaluates one document and retains its value. */
        class One : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->evaluate( fromjson( "{d:5}" ) );
                ASSERT_EQUALS( 5, accumulator()->getValue().getInt() );
            }
        };
        
        /* The accumulator evaluates one document with the field missing retains undefined. */
        class Missing : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->evaluate( fromjson( "{}" ) );
                ASSERT_EQUALS( EOO, accumulator()->getValue().getType() );
            }
        };
        
        /* The accumulator evaluates two documents and retains the maximum value. */
        class Two : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->evaluate( fromjson( "{d:5}" ) );
                accumulator()->evaluate( fromjson( "{d:7}" ) );
                ASSERT_EQUALS( 7, accumulator()->getValue().getInt() );
            }
        };
        
        /* The accumulator evaluates two documents and retains the defined value. */
        class LastMissing : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->evaluate( fromjson( "{d:7}" ) );
                accumulator()->evaluate( fromjson( "{}" ) );
                ASSERT_EQUALS( 7, accumulator()->getValue().getInt() );
            }
        };
        
    } // namespace Max

    namespace Sum {

        class Base : public AccumulatorTests::Base {
        protected:
            void createAccumulator() {
                _accumulator = AccumulatorSum::create( standalone() );
                _accumulator->addOperand( ExpressionFieldPath::create( "d" ) );
                assertBsonRepresentation( BSON( "$sum" << "$d" ), _accumulator );
            }
            Accumulator *accumulator() { return _accumulator.get(); }
        private:
            intrusive_ptr<Accumulator> _accumulator;
        };

        /** No documents evaluated. */
        class None : public Base {
        public:
            void run() {
                createAccumulator();
                ASSERT_EQUALS( 0, accumulator()->getValue().getInt() );
            }
        };

        /** An int. */
        class OneInt : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->evaluate( frombson( BSON( "d" << 5 ) ) );
                ASSERT_EQUALS( 5, accumulator()->getValue().getInt() );
            }
        };

        /** A long. */
        class OneLong : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->evaluate( frombson( BSON( "d" << 6LL ) ) );
                ASSERT_EQUALS( 6, accumulator()->getValue().getLong() );
            }
        };
        
        /** A long that cannot be expressed as an int. */
        class OneLageLong : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->evaluate( frombson( BSON( "d" << 60000000000LL ) ) );
                ASSERT_EQUALS( 60000000000LL, accumulator()->getValue().getLong() );
            }
        };
        
        /** A double. */
        class OneDouble : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->evaluate( frombson( BSON( "d" << 7.0 ) ) );
                ASSERT_EQUALS( 7.0, accumulator()->getValue().getDouble() );
            }
        };

        /** A non integer valued double. */
        class OneFractionalDouble : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->evaluate( frombson( BSON( "d" << 7.5 ) ) );
                ASSERT_EQUALS( 7.5, accumulator()->getValue().getDouble() );
            }
        };

        /** A nan double. */
        class OneNanDouble : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->evaluate
                        ( frombson( BSON( "d" << numeric_limits<double>::quiet_NaN() ) ) );
                // NaN is unequal to itself.
                ASSERT_NOT_EQUALS( accumulator()->getValue().getDouble(),
                                   accumulator()->getValue().getDouble() );
            }
        };
        
        class TypeConversionBase : public Base {
        public:
            virtual ~TypeConversionBase() {
            }
            void run() {
                checkPairSum( summand1(), summand2() );
                checkPairSum( summand2(), summand1() );
            }
        protected:
            virtual BSONObj summand1() { verify( false ); }
            virtual BSONObj summand2() { verify( false ); }
            virtual BSONObj expectedSum() = 0;
            void checkPairSum( BSONObj first, BSONObj second ) {
                Document firstDocument = Document::createFromBsonObj( &first );
                Document secondDocument = Document::createFromBsonObj( &second );
                createAccumulator();
                accumulator()->evaluate( firstDocument );
                accumulator()->evaluate( secondDocument );
                checkSum();
            }
            void checkSum() {
                BSONObjBuilder resultBuilder;
                accumulator()->getValue().addToBsonObj( &resultBuilder, "" );
                BSONObj result = resultBuilder.obj();
                ASSERT_EQUALS( expectedSum().firstElement(), result.firstElement() );                
                ASSERT_EQUALS( expectedSum().firstElement().type(), result.firstElement().type() );
            }
        };

        /** Two ints are summed. */
        class IntInt : public TypeConversionBase {
            BSONObj summand1() { return BSON( "d" << 4 ); }
            BSONObj summand2() { return BSON( "d" << 5 ); }
            BSONObj expectedSum() { return BSON( "" << 9 ); }
        };

        /** Two ints overflow. */
        class IntIntOverflow : public TypeConversionBase {
            BSONObj summand1() { return BSON( "d" << numeric_limits<int>::max() ); }
            BSONObj summand2() { return BSON( "d" << 10 ); }
            BSONObj expectedSum() {
                return BSON( "" << numeric_limits<int>::max() + 10LL );
            }
        };

        /** Two ints negative overflow. */
        class IntIntNegativeOverflow : public TypeConversionBase {
            BSONObj summand1() { return BSON( "d" << -numeric_limits<int>::max() ); }
            BSONObj summand2() { return BSON( "d" << -10 ); }
            BSONObj expectedSum() {
                return BSON( "" << -numeric_limits<int>::max() + -10LL );
            }
        };
        
        /** An int and a long are summed. */
        class IntLong : public TypeConversionBase {
            BSONObj summand1() { return BSON( "d" << 4 ); }
            BSONObj summand2() { return BSON( "d" << 5LL ); }
            BSONObj expectedSum() { return BSON( "" << 9LL ); }
        };
        
        /** An int and a long do not trigger an int overflow. */
        class IntLongNoIntOverflow : public TypeConversionBase {
            BSONObj summand1() { return BSON( "d" << numeric_limits<int>::max() ); }
            BSONObj summand2() { return BSON( "d" << 1LL ); }
            BSONObj expectedSum() {
                return BSON( "" << (long long)numeric_limits<int>::max() + 1 );
            }
        };
        
        /** An int and a long overflow. */
        class IntLongLongOverflow : public TypeConversionBase {
            BSONObj summand1() { return BSON( "d" << 1 ); }
            BSONObj summand2() { return BSON( "d" << numeric_limits<long long>::max() ); }
            BSONObj expectedSum() { return BSON( "" << numeric_limits<long long>::max() + 1 ); }
        };

        /** Two longs are summed. */
        class LongLong : public TypeConversionBase {
            BSONObj summand1() { return BSON( "d" << 4LL ); }
            BSONObj summand2() { return BSON( "d" << 5LL ); }
            BSONObj expectedSum() { return BSON( "" << 9LL ); }            
        };
        
        /** Two longs overflow. */
        class LongLongOverflow : public TypeConversionBase {
            BSONObj summand1() { return BSON( "d" << numeric_limits<long long>::max() ); }
            BSONObj summand2() { return BSON( "d" << numeric_limits<long long>::max() ); }
            BSONObj expectedSum() {
                return BSON( "" << numeric_limits<long long>::max() +
                        numeric_limits<long long>::max() );
            }
        };
        
        /** An int and a double are summed. */
        class IntDouble : public TypeConversionBase {
            BSONObj summand1() { return BSON( "d" << 4 ); }
            BSONObj summand2() { return BSON( "d" << 5.5 ); }
            BSONObj expectedSum() { return BSON( "" << 9.5 ); }
        };
        
        /** An int and a NaN double are summed. */
        class IntNanDouble : public TypeConversionBase {
            BSONObj summand1() { return BSON( "d" << 4 ); }
            BSONObj summand2() { return BSON( "d" << numeric_limits<double>::quiet_NaN() ); }
            BSONObj expectedSum() {
                // BSON compares NaN values as equal.
                return BSON( "" << numeric_limits<double>::quiet_NaN() );
            }
        };
        
        /** An int and a NaN sum to NaN. */
        class IntDoubleNoIntOverflow : public TypeConversionBase {
            BSONObj summand1() { return BSON( "d" << numeric_limits<int>::max() ); }
            BSONObj summand2() { return BSON( "d" << 1.0 ); }
            BSONObj expectedSum() {
                return BSON( "" << (long long)numeric_limits<int>::max() + 1.0 );
            }
        };
        
        /** A long and a double are summed. */
        class LongDouble : public TypeConversionBase {
            BSONObj summand1() { return BSON( "d" << 4LL ); }
            BSONObj summand2() { return BSON( "d" << 5.5 ); }
            BSONObj expectedSum() { return BSON( "" << 9.5 ); }
        };
        
        /** A long and a double do not trigger a long overflow. */
        class LongDoubleNoLongOverflow : public TypeConversionBase {
            BSONObj summand1() { return BSON( "d" << numeric_limits<long long>::max() ); }
            BSONObj summand2() { return BSON( "d" << 1.0 ); }
            BSONObj expectedSum() {
                return BSON( "" << (long long)numeric_limits<long long>::max() + 1.0 );
            }
        };

        /** Two double values are summed. */
        class DoubleDouble : public TypeConversionBase {
            BSONObj summand1() { return BSON( "d" << 2.5 ); }
            BSONObj summand2() { return BSON( "d" << 5.5 ); }
            BSONObj expectedSum() { return BSON( "" << 8.0 ); }
        };

        /** Two double values overflow. */
        class DoubleDoubleOverflow : public TypeConversionBase {
            BSONObj summand1() { return BSON( "d" << numeric_limits<double>::max() ); }
            BSONObj summand2() { return BSON( "d" << numeric_limits<double>::max() ); }
            BSONObj expectedSum() { return BSON( "" << numeric_limits<double>::infinity() ); }
        };

        /** Three values, an int, a long, and a double, are summed. */
        class IntLongDouble : public TypeConversionBase {
        public:
            void run() {
                createAccumulator();
                accumulator()->evaluate( frombson( BSON( "d" << 5 ) ) );
                accumulator()->evaluate( frombson( BSON( "d" << 99 ) ) );
                accumulator()->evaluate( frombson( BSON( "d" << 0.2 ) ) );
                checkSum();
            }
        private:
            BSONObj expectedSum() { return BSON( "" << 104.2 ); }
        };

        /** A negative value is summed. */
        class Negative : public TypeConversionBase {
            BSONObj summand1() { return BSON( "d" << 5 ); }
            BSONObj summand2() { return BSON( "d" << -8.8 ); }
            BSONObj expectedSum() { return BSON( "" << 5 - 8.8 ); }
        };

        /** A long and a negative int are is summed. */
        class LongIntNegative : public TypeConversionBase {
            BSONObj summand1() { return BSON( "d" << 5LL ); }
            BSONObj summand2() { return BSON( "d" << -6 ); }
            BSONObj expectedSum() { return BSON( "" << -1LL ); }
        };
        
        /** A null value is summed as zero. */
        class IntNull : public TypeConversionBase {
            BSONObj summand1() { return BSON( "d" << 5 ); }
            BSONObj summand2() { return BSON( "d" << BSONNULL ); }
            BSONObj expectedSum() { return BSON( "" << 5 ); }
        };

        /** An undefined value is summed as zero. */
        class IntUndefined : public TypeConversionBase {
            BSONObj summand1() { return BSON( "d" << 9 ); }
            BSONObj summand2() { return BSONObj(); }
            BSONObj expectedSum() { return BSON( "" << 9 ); }
        };

        /** Two large integers do not overflow if a double is added later. */
        class NoOverflowBeforeDouble : public TypeConversionBase {
        public:
            void run() {
                createAccumulator();
                accumulator()->evaluate
                        ( frombson( BSON( "d" << numeric_limits<long long>::max() ) ) );
                accumulator()->evaluate
                        ( frombson( BSON( "d" << numeric_limits<long long>::max() ) ) );
                accumulator()->evaluate( frombson( BSON( "d" << 1.0 ) ) );
                checkSum();
            }
        private:
            BSONObj expectedSum() {
                return BSON( "" << (double)numeric_limits<long long>::max() +
                             (double)numeric_limits<long long>::max() );
            }
        };
        
    } // namespace Sum

    class All : public Suite {
    public:
        All() : Suite( "accumulator" ) {
        }
        void setupTests() {
            add<Avg::None>();
            add<Avg::OneInt>();
            add<Avg::OneLong>();
            add<Avg::OneDouble>();
            add<Avg::IntInt>();
            add<Avg::IntDouble>();
            add<Avg::IntIntNoOverflow>();
            add<Avg::LongLongOverflow>();
            add<Avg::Shard::Int>();
            add<Avg::Shard::Long>();
            add<Avg::Shard::Double>();
            add<Avg::Shard::IntIntOverflow>();
            add<Avg::Shard::IntLong>();
            add<Avg::Shard::IntDouble>();
            add<Avg::Shard::LongDouble>();
            add<Avg::Shard::IntLongDouble>();
            add<Avg::Router::OneShard>();
            add<Avg::Router::TwoShards>();

            add<First::None>();
            add<First::One>();
            add<First::Missing>();
            add<First::Two>();
            add<First::FirstMissing>();

            add<Last::None>();
            add<Last::One>();
            add<Last::Missing>();
            add<Last::Two>();
            add<Last::LastMissing>();

            add<Min::None>();
            add<Min::One>();
            add<Min::Missing>();
            add<Min::Two>();
            add<Min::LastMissing>();

            add<Max::None>();
            add<Max::One>();
            add<Max::Missing>();
            add<Max::Two>();
            add<Max::LastMissing>();

            add<Sum::None>();
            add<Sum::OneInt>();
            add<Sum::OneLong>();
            add<Sum::OneLageLong>();
            add<Sum::OneDouble>();
            add<Sum::OneFractionalDouble>();
            add<Sum::OneNanDouble>();
            add<Sum::IntInt>();
            add<Sum::IntIntOverflow>();
            add<Sum::IntIntNegativeOverflow>();
            add<Sum::IntLong>();
            add<Sum::IntLongNoIntOverflow>();
            add<Sum::IntLongLongOverflow>();
            add<Sum::LongLong>();
            add<Sum::LongLongOverflow>();
            add<Sum::IntDouble>();
            add<Sum::IntNanDouble>();
            add<Sum::IntDoubleNoIntOverflow>();
            add<Sum::LongDouble>();
            add<Sum::LongDoubleNoLongOverflow>();
            add<Sum::DoubleDouble>();
            add<Sum::DoubleDoubleOverflow>();
            add<Sum::IntLongDouble>();
            add<Sum::Negative>();
            add<Sum::LongIntNegative>();
            add<Sum::IntNull>();
            add<Sum::IntUndefined>();
            add<Sum::NoOverflowBeforeDouble>();
        }
    } myall;

} // namespace AccumulatorTests
