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

#include "mongo/pch.h"

#include "mongo/db/interrupt_status_mongod.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/dbtests/dbtests.h"

namespace AccumulatorTests {

    class Base {
    protected:
        BSONObj fromDocument( const Document& document ) {
            return document.toBson();
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
    private:
        intrusive_ptr<ExpressionContext> _shard;
        intrusive_ptr<ExpressionContext> _router;        
    };

    namespace Avg {
        
        class Base : public AccumulatorTests::Base {
        public:
            virtual ~Base() {
            }
        protected:
            void createAccumulator() {
                _accumulator = AccumulatorAvg::create();
                ASSERT_EQUALS(string("$avg"), _accumulator->getOpName());
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
                ASSERT_EQUALS( 0, accumulator()->getValue(false).getDouble() );
            }
        };
        
        /** One int value is converted to double. */
        class OneInt : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->process(Value(3), false);
                ASSERT_EQUALS( 3, accumulator()->getValue(false).getDouble() );
            }
        };
        
        /** One long value is converted to double. */
        class OneLong : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->process(Value(-4LL), false);
                ASSERT_EQUALS( -4, accumulator()->getValue(false).getDouble() );
            }
        };
        
        /** One double value. */
        class OneDouble : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->process(Value(22.6), false);
                ASSERT_EQUALS( 22.6, accumulator()->getValue(false).getDouble() );
            }
        };
        
        /** The average of two ints is an int, even if inexact. */
        class IntInt : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->process(Value(10), false);
                accumulator()->process(Value(11), false);
                ASSERT_EQUALS( 10.5, accumulator()->getValue(false).getDouble() );
            }
        };        
        
        /** The average of an int and a double is calculated as a double. */
        class IntDouble : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->process(Value(10), false);
                accumulator()->process(Value(11.0), false);
                ASSERT_EQUALS( 10.5, accumulator()->getValue(false).getDouble() );
            }
        };        
        
        /** Unlike $sum, two ints do not overflow in the 'total' portion of the average. */
        class IntIntNoOverflow : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->process(Value(numeric_limits<int>::max()), false);
                accumulator()->process(Value(numeric_limits<int>::max()), false);
                ASSERT_EQUALS(numeric_limits<int>::max(),
                              accumulator()->getValue(false).getDouble());
            }
        };        
        
        /** Two longs do overflow in the 'total' portion of the average. */
        class LongLongOverflow : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->process(Value(numeric_limits<long long>::max()), false);
                accumulator()->process(Value(numeric_limits<long long>::max()), false);
                ASSERT_EQUALS( ( (double)numeric_limits<long long>::max() +
                                 numeric_limits<long long>::max() ) / 2.0,
                               accumulator()->getValue(false).getDouble() );
            }
        };

        namespace Shard {
            class SingleOperandBase : public Base {
            public:
                void run() {
                    createAccumulator();
                    accumulator()->process(operand(), false);
                    assertBinaryEqual( expectedResult(),
                                       fromDocument(accumulator()->getValue(true).getDocument()));
                }
            protected:
                virtual Value operand() = 0;
                virtual BSONObj expectedResult() = 0;
            };

            /** Shard result for one integer. */
            class Int : public SingleOperandBase {
                Value operand() { return Value(3); }
                BSONObj expectedResult() { return BSON( "subTotal" << 3.0 << "count" << 1LL ); }
            };
            
            /** Shard result for one long. */
            class Long : public SingleOperandBase {
                Value operand() { return Value(5LL); }
                BSONObj expectedResult() { return BSON( "subTotal" << 5.0 << "count" << 1LL ); }
            };
            
            /** Shard result for one double. */
            class Double : public SingleOperandBase {
                Value operand() { return Value(116.0); }
                BSONObj expectedResult() { return BSON( "subTotal" << 116.0 << "count" << 1LL ); }
            };

            class TwoOperandBase : public Base {
            public:
                void run() {
                    checkAvg( operand1(), operand2() );
                    checkAvg( operand2(), operand1() );
                }
            protected:
                virtual Value operand1() = 0;
                virtual Value operand2() = 0;
                virtual BSONObj expectedResult() = 0;
            private:
                void checkAvg( const Value& a, const Value& b ) {
                    createAccumulator();
                    accumulator()->process(a, false);
                    accumulator()->process(b, false);
                    assertBinaryEqual(expectedResult(),
                                      fromDocument(accumulator()->getValue(true).getDocument()));
                }
            };

            /** Shard two ints overflow. */
            class IntIntOverflow : public TwoOperandBase {
                Value operand1() { return Value(numeric_limits<int>::max()); }
                Value operand2() { return Value(3); }
                BSONObj expectedResult() {
                    return BSON( "subTotal" << numeric_limits<int>::max() + 3.0 << "count" << 2LL );
                }
            };

            /** Shard avg an int and a long. */
            class IntLong : public TwoOperandBase {
                Value operand1() { return Value(5); }
                Value operand2() { return Value(3LL); }
                BSONObj expectedResult() { return BSON( "subTotal" << 8.0 << "count" << 2LL ); }
            };
            
            /** Shard avg an int and a double. */
            class IntDouble : public TwoOperandBase {
                Value operand1() { return Value(5); }
                Value operand2() { return Value(6.2); }
                BSONObj expectedResult() { return BSON( "subTotal" << 11.2 << "count" << 2LL ); }
            };
            
            /** Shard avg a long and a double. */
            class LongDouble : public TwoOperandBase {
                Value operand1() { return Value(5LL); }
                Value operand2() { return Value(1.0); }
                BSONObj expectedResult() { return BSON( "subTotal" << 6.0 << "count" << 2LL ); }
            };

            /** Shard avg an int, long, and double. */
            class IntLongDouble : public Base {
            public:
                void run() {
                    createAccumulator();
                    accumulator()->process(Value(1), false);
                    accumulator()->process(Value(2LL), false);
                    accumulator()->process(Value(4.0), false);
                    assertBinaryEqual(BSON( "subTotal" << 7.0 << "count" << 3LL ),
                                      fromDocument(accumulator()->getValue(true).getDocument()));
                }
            };

        } // namespace Shard

        namespace Router {
            /** Router result from one shard. */
            class OneShard : public Base {
            public:
                void run() {
                    createAccumulator();
                    accumulator()->process(Value(DOC("subTotal" << 3.0 << "count" << 2LL)), true);
                    assertBinaryEqual( BSON( "" << 3.0 / 2 ),
                                       fromValue( accumulator()->getValue(false) ) );
                }
            };
            
            /** Router result from two shards. */
            class TwoShards : public Base {
            public:
                void run() {
                    createAccumulator();
                    accumulator()->process(Value(DOC("subTotal" << 6.0 << "count" << 1LL)), true);
                    accumulator()->process(Value(DOC("subTotal" << 5.0 << "count" << 2LL)), true);
                    assertBinaryEqual( BSON( "" << 11.0 / 3 ),
                                       fromValue( accumulator()->getValue(false) ) );
                }
            };

        } // namespace Router
        
    } // namespace Avg

    namespace First {

        class Base : public AccumulatorTests::Base {
        protected:
            void createAccumulator() {
                _accumulator = AccumulatorFirst::create();
                ASSERT_EQUALS(string("$first"), _accumulator->getOpName());
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
                ASSERT( accumulator()->getValue(false).missing() );
            }
        };

        /* The accumulator evaluates one document and retains its value. */
        class One : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->process(Value(5), false);
                ASSERT_EQUALS( 5, accumulator()->getValue(false).getInt() );
            }
        };
        
        /* The accumulator evaluates one document with the field missing, returns missing value. */
        class Missing : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->process(Value(), false);
                ASSERT_EQUALS( EOO, accumulator()->getValue(false).getType() );
            }
        };
        
        /* The accumulator evaluates two documents and retains the value in the first. */
        class Two : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->process(Value(5), false);
                accumulator()->process(Value(7), false);
                ASSERT_EQUALS( 5, accumulator()->getValue(false).getInt() );
            }
        };
        
        /* The accumulator evaluates two documents and retains the missing value in the first. */
        class FirstMissing : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->process(Value(), false);
                accumulator()->process(Value(7), false);
                ASSERT_EQUALS( EOO, accumulator()->getValue(false).getType() );
            }
        };
        
    } // namespace First

    namespace Last {
        
        class Base : public AccumulatorTests::Base {
        protected:
            void createAccumulator() {
                _accumulator = AccumulatorLast::create();
                ASSERT_EQUALS(string("$last"), _accumulator->getOpName());
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
                ASSERT( accumulator()->getValue(false).missing() );
            }
        };
        
        /* The accumulator evaluates one document and retains its value. */
        class One : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->process(Value(5), false);
                ASSERT_EQUALS( 5, accumulator()->getValue(false).getInt() );
            }
        };
        
        /* The accumulator evaluates one document with the field missing retains undefined. */
        class Missing : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->process(Value(), false);
                ASSERT_EQUALS( EOO , accumulator()->getValue(false).getType() );
            }
        };
        
        /* The accumulator evaluates two documents and retains the value in the last. */
        class Two : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->process(Value(5), false);
                accumulator()->process(Value(7), false);
                ASSERT_EQUALS( 7, accumulator()->getValue(false).getInt() );
            }
        };
        
        /* The accumulator evaluates two documents and retains the undefined value in the last. */
        class LastMissing : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->process(Value(7), false);
                accumulator()->process(Value(), false);
                ASSERT_EQUALS( EOO , accumulator()->getValue(false).getType() );
            }
        };
        
    } // namespace Last
    
    namespace Min {
        
        class Base : public AccumulatorTests::Base {
        protected:
            void createAccumulator() {
                _accumulator = AccumulatorMinMax::createMin();
                ASSERT_EQUALS(string("$min"), _accumulator->getOpName());
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
                ASSERT( accumulator()->getValue(false).missing() );
            }
        };
        
        /* The accumulator evaluates one document and retains its value. */
        class One : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->process(Value(5), false);
                ASSERT_EQUALS( 5, accumulator()->getValue(false).getInt() );
            }
        };
        
        /* The accumulator evaluates one document with the field missing retains undefined. */
        class Missing : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->process(Value(), false);
                ASSERT_EQUALS( EOO , accumulator()->getValue(false).getType() );
            }
        };
        
        /* The accumulator evaluates two documents and retains the minimum value. */
        class Two : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->process(Value(5), false);
                accumulator()->process(Value(7), false);
                ASSERT_EQUALS( 5, accumulator()->getValue(false).getInt() );
            }
        };
        
        /* The accumulator evaluates two documents and retains the undefined value. */
        class LastMissing : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->process(Value(7), false);
                accumulator()->process(Value(), false);
                ASSERT_EQUALS( 7 , accumulator()->getValue(false).getInt() );
            }
        };
        
    } // namespace Min
    
    namespace Max {
        
        class Base : public AccumulatorTests::Base {
        protected:
            void createAccumulator() {
                _accumulator = AccumulatorMinMax::createMax();
                ASSERT_EQUALS(string("$max"), _accumulator->getOpName());
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
                ASSERT( accumulator()->getValue(false).missing() );
            }
        };
        
        /* The accumulator evaluates one document and retains its value. */
        class One : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->process(Value(5), false);
                ASSERT_EQUALS( 5, accumulator()->getValue(false).getInt() );
            }
        };
        
        /* The accumulator evaluates one document with the field missing retains undefined. */
        class Missing : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->process(Value(), false);
                ASSERT_EQUALS( EOO, accumulator()->getValue(false).getType() );
            }
        };
        
        /* The accumulator evaluates two documents and retains the maximum value. */
        class Two : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->process(Value(5), false);
                accumulator()->process(Value(7), false);
                ASSERT_EQUALS( 7, accumulator()->getValue(false).getInt() );
            }
        };
        
        /* The accumulator evaluates two documents and retains the defined value. */
        class LastMissing : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->process(Value(7), false);
                accumulator()->process(Value(), false);
                ASSERT_EQUALS( 7, accumulator()->getValue(false).getInt() );
            }
        };
        
    } // namespace Max

    namespace Sum {

        class Base : public AccumulatorTests::Base {
        protected:
            void createAccumulator() {
                _accumulator = AccumulatorSum::create();
                ASSERT_EQUALS(string("$sum"), _accumulator->getOpName());
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
                ASSERT_EQUALS( 0, accumulator()->getValue(false).getInt() );
            }
        };

        /** An int. */
        class OneInt : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->process(Value(5), false);
                ASSERT_EQUALS( 5, accumulator()->getValue(false).getInt() );
            }
        };

        /** A long. */
        class OneLong : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->process(Value(6LL), false);
                ASSERT_EQUALS( 6, accumulator()->getValue(false).getLong() );
            }
        };
        
        /** A long that cannot be expressed as an int. */
        class OneLageLong : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->process(Value(60000000000LL), false);
                ASSERT_EQUALS( 60000000000LL, accumulator()->getValue(false).getLong() );
            }
        };
        
        /** A double. */
        class OneDouble : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->process(Value(7.0), false);
                ASSERT_EQUALS( 7.0, accumulator()->getValue(false).getDouble() );
            }
        };

        /** A non integer valued double. */
        class OneFractionalDouble : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->process(Value(7.5), false);
                ASSERT_EQUALS( 7.5, accumulator()->getValue(false).getDouble() );
            }
        };

        /** A nan double. */
        class OneNanDouble : public Base {
        public:
            void run() {
                createAccumulator();
                accumulator()->process(Value(numeric_limits<double>::quiet_NaN()), false);
                // NaN is unequal to itself.
                ASSERT_NOT_EQUALS( accumulator()->getValue(false).getDouble(),
                                   accumulator()->getValue(false).getDouble() );
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
            virtual Value summand1() { verify( false ); }
            virtual Value summand2() { verify( false ); }
            virtual Value expectedSum() = 0;
            void checkPairSum( Value first, Value second ) {
                createAccumulator();
                accumulator()->process(first, false);
                accumulator()->process(second, false);
                checkSum();
            }
            void checkSum() {
                Value result = accumulator()->getValue(false);
                ASSERT_EQUALS( expectedSum(), result );                
                ASSERT_EQUALS( expectedSum().getType(), result.getType() );
            }
        };

        /** Two ints are summed. */
        class IntInt : public TypeConversionBase {
            Value summand1() { return Value(4); }
            Value summand2() { return Value(5); }
            Value expectedSum() { return Value(9); }
        };

        /** Two ints overflow. */
        class IntIntOverflow : public TypeConversionBase {
            Value summand1() { return Value(numeric_limits<int>::max()); }
            Value summand2() { return Value(10); }
            Value expectedSum() { return Value(numeric_limits<int>::max() + 10LL); }
        };

        /** Two ints negative overflow. */
        class IntIntNegativeOverflow : public TypeConversionBase {
            Value summand1() { return Value(-numeric_limits<int>::max()); }
            Value summand2() { return Value(-10); }
            Value expectedSum() { return Value(-numeric_limits<int>::max() + -10LL); }
        };
        
        /** An int and a long are summed. */
        class IntLong : public TypeConversionBase {
            Value summand1() { return Value(4); }
            Value summand2() { return Value(5LL); }
            Value expectedSum() { return Value(9LL); }
        };
        
        /** An int and a long do not trigger an int overflow. */
        class IntLongNoIntOverflow : public TypeConversionBase {
            Value summand1() { return Value(numeric_limits<int>::max()); }
            Value summand2() { return Value(1LL); }
            Value expectedSum() { return Value((long long)numeric_limits<int>::max() + 1); }
        };
        
        /** An int and a long overflow. */
        class IntLongLongOverflow : public TypeConversionBase {
            Value summand1() { return Value(1); }
            Value summand2() { return Value(numeric_limits<long long>::max()); }
            Value expectedSum() { return Value(numeric_limits<long long>::max() + 1); }
        };

        /** Two longs are summed. */
        class LongLong : public TypeConversionBase {
            Value summand1() { return Value(4LL); }
            Value summand2() { return Value(5LL); }
            Value expectedSum() { return Value(9LL); }            
        };
        
        /** Two longs overflow. */
        class LongLongOverflow : public TypeConversionBase {
            Value summand1() { return Value(numeric_limits<long long>::max()); }
            Value summand2() { return Value(numeric_limits<long long>::max()); }
            Value expectedSum() {
                return Value(numeric_limits<long long>::max()
                           + numeric_limits<long long>::max());
            }
        };
        
        /** An int and a double are summed. */
        class IntDouble : public TypeConversionBase {
            Value summand1() { return Value(4); }
            Value summand2() { return Value(5.5); }
            Value expectedSum() { return Value(9.5); }
        };
        
        /** An int and a NaN double are summed. */
        class IntNanDouble : public TypeConversionBase {
            Value summand1() { return Value(4); }
            Value summand2() { return Value(numeric_limits<double>::quiet_NaN()); }
            Value expectedSum() {
                // BSON compares NaN values as equal.
                return Value(numeric_limits<double>::quiet_NaN());
            }
        };
        
        /** An int and a NaN sum to NaN. */
        class IntDoubleNoIntOverflow : public TypeConversionBase {
            Value summand1() { return Value(numeric_limits<int>::max()); }
            Value summand2() { return Value(1.0); }
            Value expectedSum() {
                return Value((long long)numeric_limits<int>::max() + 1.0);
            }
        };
        
        /** A long and a double are summed. */
        class LongDouble : public TypeConversionBase {
            Value summand1() { return Value(4LL); }
            Value summand2() { return Value(5.5); }
            Value expectedSum() { return Value(9.5); }
        };
        
        /** A long and a double do not trigger a long overflow. */
        class LongDoubleNoLongOverflow : public TypeConversionBase {
            Value summand1() { return Value(numeric_limits<long long>::max()); }
            Value summand2() { return Value(1.0); }
            Value expectedSum() {
                return Value((long long)numeric_limits<long long>::max() + 1.0);
            }
        };

        /** Two double values are summed. */
        class DoubleDouble : public TypeConversionBase {
            Value summand1() { return Value(2.5); }
            Value summand2() { return Value(5.5); }
            Value expectedSum() { return Value(8.0); }
        };

        /** Two double values overflow. */
        class DoubleDoubleOverflow : public TypeConversionBase {
            Value summand1() { return Value(numeric_limits<double>::max()); }
            Value summand2() { return Value(numeric_limits<double>::max()); }
            Value expectedSum() { return Value(numeric_limits<double>::infinity()); }
        };

        /** Three values, an int, a long, and a double, are summed. */
        class IntLongDouble : public TypeConversionBase {
        public:
            void run() {
                createAccumulator();
                accumulator()->process(Value(5), false);
                accumulator()->process(Value(99), false);
                accumulator()->process(Value(0.2), false);
                checkSum();
            }
        private:
            Value expectedSum() { return Value(104.2); }
        };

        /** A negative value is summed. */
        class Negative : public TypeConversionBase {
            Value summand1() { return Value(5); }
            Value summand2() { return Value(-8.8); }
            Value expectedSum() { return Value(5 - 8.8); }
        };

        /** A long and a negative int are is summed. */
        class LongIntNegative : public TypeConversionBase {
            Value summand1() { return Value(5LL); }
            Value summand2() { return Value(-6); }
            Value expectedSum() { return Value(-1LL); }
        };
        
        /** A null value is summed as zero. */
        class IntNull : public TypeConversionBase {
            Value summand1() { return Value(5); }
            Value summand2() { return Value(BSONNULL); }
            Value expectedSum() { return Value(5); }
        };

        /** An undefined value is summed as zero. */
        class IntUndefined : public TypeConversionBase {
            Value summand1() { return Value(9); }
            Value summand2() { return Value(); }
            Value expectedSum() { return Value(9); }
        };

        /** Two large integers do not overflow if a double is added later. */
        class NoOverflowBeforeDouble : public TypeConversionBase {
        public:
            void run() {
                createAccumulator();
                accumulator()->process(Value(numeric_limits<long long>::max()), false);
                accumulator()->process(Value(numeric_limits<long long>::max()), false);
                accumulator()->process(Value(1.0), false);
                checkSum();
            }
        private:
            Value expectedSum() {
                return Value((double)numeric_limits<long long>::max()
                           + (double)numeric_limits<long long>::max());
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
