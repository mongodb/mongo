// pipelinetests.cpp : Unit tests for some classes within src/mongo/db/pipeline.

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
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/pch.h"

#include "mongo/db/interrupt_status.h"
#include "mongo/db/interrupt_status_mongod.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/dbtests/dbtests.h"

namespace PipelineTests {

    namespace FieldPath {

        using mongo::FieldPath;

        /** FieldPath constructed from empty string. */
        class Empty {
        public:
            void run() {
                ASSERT_THROWS( FieldPath path( "" ), UserException );
            }
        };

        /** FieldPath constructed from empty vector. */
        class EmptyVector {
        public:
            void run() {
                vector<string> vec;
                ASSERT_THROWS( FieldPath path( vec ), MsgAssertionException );
            }
        };

        /** FieldPath constructed from a simple string (without dots). */
        class Simple {
        public:
            void run() {
                FieldPath path( "foo" );
                ASSERT_EQUALS( 1U, path.getPathLength() );
                ASSERT_EQUALS( "foo", path.getFieldName( 0 ) );
                ASSERT_EQUALS( "foo", path.getPath( false ) );
                ASSERT_EQUALS( "$foo", path.getPath( true ) );
            }
        };

        /** FieldPath constructed from a single element vector. */
        class SimpleVector {
        public:
            void run() {
                vector<string> vec( 1, "foo" );
                FieldPath path( vec );
                ASSERT_EQUALS( 1U, path.getPathLength() );
                ASSERT_EQUALS( "foo", path.getFieldName( 0 ) );
                ASSERT_EQUALS( "foo", path.getPath( false ) );
            }
        };
        
        /** FieldPath consisting of a '$' character. */
        class DollarSign {
        public:
            void run() {
                ASSERT_THROWS( FieldPath path( "$" ), UserException );
            }
        };

        /** FieldPath with a '$' prefix. */
        class DollarSignPrefix {
        public:
            void run() {
                ASSERT_THROWS( FieldPath path( "$a" ), UserException );
            }
        };
        
        /** FieldPath constructed from a string with one dot. */
        class Dotted {
        public:
            void run() {
                FieldPath path( "foo.bar" );
                ASSERT_EQUALS( 2U, path.getPathLength() );
                ASSERT_EQUALS( "foo", path.getFieldName( 0 ) );
                ASSERT_EQUALS( "bar", path.getFieldName( 1 ) );
                ASSERT_EQUALS( "foo.bar", path.getPath( false ) );
                ASSERT_EQUALS( "$foo.bar", path.getPath( true ) );
            }
        };

        /** FieldPath constructed from a single element vector containing a dot. */
        class VectorWithDot {
        public:
            void run() {
                vector<string> vec( 1, "fo.o" );
                ASSERT_THROWS( FieldPath path( vec ), UserException );
            }
        };

        /** FieldPath constructed from a two element vector. */
        class TwoFieldVector {
        public:
            void run() {
                vector<string> vec;
                vec.push_back( "foo" );
                vec.push_back( "bar" );
                FieldPath path( vec );
                ASSERT_EQUALS( 2U, path.getPathLength() );
                ASSERT_EQUALS( "foo.bar", path.getPath( false ) );
            }
        };

        /** FieldPath with a '$' prefix in the second field. */
        class DollarSignPrefixSecondField {
        public:
            void run() {
                ASSERT_THROWS( FieldPath path( "a.$b" ), UserException );
            }
        };

        /** FieldPath constructed from a string with two dots. */
        class TwoDotted {
        public:
            void run() {
                FieldPath path( "foo.bar.baz" );
                ASSERT_EQUALS( 3U, path.getPathLength() );
                ASSERT_EQUALS( "foo", path.getFieldName( 0 ) );
                ASSERT_EQUALS( "bar", path.getFieldName( 1 ) );
                ASSERT_EQUALS( "baz", path.getFieldName( 2 ) );
                ASSERT_EQUALS( "foo.bar.baz", path.getPath( false ) );
            }
        };

        /** FieldPath constructed from a string ending in a dot. */
        class TerminalDot {
        public:
            void run() {
                ASSERT_THROWS( FieldPath path( "foo." ), UserException );
            }
        };

        /** FieldPath constructed from a string beginning with a dot. */
        class PrefixDot {
        public:
            void run() {
                ASSERT_THROWS( FieldPath path( ".foo" ), UserException );
            }
        };

        /** FieldPath constructed from a string with adjacent dots. */
        class AdjacentDots {
        public:
            void run() {
                ASSERT_THROWS( FieldPath path( "foo..bar" ), UserException );
            }
        };

        /** FieldPath constructed from a string with one letter between two dots. */
        class LetterBetweenDots {
        public:
            void run() {
                FieldPath path( "foo.a.bar" );
                ASSERT_EQUALS( 3U, path.getPathLength() );
                ASSERT_EQUALS( "foo.a.bar", path.getPath( false ) );                
            }
        };

        /** FieldPath containing a null character. */
        class NullCharacter {
        public:
            void run() {
                ASSERT_THROWS( FieldPath path( string( "foo.b\0r", 7 ) ), UserException );                
            }
        };
        
        /** FieldPath constructed with a vector containing a null character. */
        class VectorNullCharacter {
        public:
            void run() {
                vector<string> vec;
                vec.push_back( "foo" );
                vec.push_back( string( "b\0r", 3 ) );
                ASSERT_THROWS( FieldPath path( vec ), UserException );                
            }
        };

        /** Tail of a FieldPath. */
        class Tail {
        public:
            void run() {
                FieldPath path = FieldPath( "foo.bar" ).tail();
                ASSERT_EQUALS( 1U, path.getPathLength() );
                ASSERT_EQUALS( "bar", path.getPath( false ) );                
            }
        };
        
        /** Tail of a FieldPath with three fields. */
        class TailThreeFields {
        public:
            void run() {
                FieldPath path = FieldPath( "foo.bar.baz" ).tail();
                ASSERT_EQUALS( 2U, path.getPathLength() );
                ASSERT_EQUALS( "bar.baz", path.getPath( false ) );                
            }
        };
        
    } // namespace FieldPath

    namespace Optimizations {
        using namespace mongo;

        namespace Sharded {
            class Base {
            public:
                // These all return json arrays of pipeline operators
                virtual string inputPipeJson() = 0;
                virtual string shardPipeJson() = 0;
                virtual string mergePipeJson() = 0;

                BSONObj pipelineFromJsonArray(const string& array) {
                    return fromjson("{pipeline: " + array + "}");
                }
                virtual void run() {
                    const BSONObj inputBson = pipelineFromJsonArray(inputPipeJson());
                    const BSONObj shardPipeExpected = pipelineFromJsonArray(shardPipeJson());
                    const BSONObj mergePipeExpected = pipelineFromJsonArray(mergePipeJson());

                    intrusive_ptr<ExpressionContext> ctx =
                        new ExpressionContext(InterruptStatusMongod::status,
                                              NamespaceString("a.collection"));
                    string errmsg;
                    intrusive_ptr<Pipeline> mergePipe =
                        Pipeline::parseCommand(errmsg, inputBson, ctx);
                    ASSERT_EQUALS(errmsg, "");
                    ASSERT(mergePipe != NULL);

                    intrusive_ptr<Pipeline> shardPipe = mergePipe->splitForSharded();
                    ASSERT(shardPipe != NULL);

                    ASSERT_EQUALS(shardPipe->serialize()["pipeline"],
                                  Value(shardPipeExpected["pipeline"]));
                    ASSERT_EQUALS(mergePipe->serialize()["pipeline"],
                                  Value(mergePipeExpected["pipeline"]));
                }

                virtual ~Base() {};
            };

            // General test to make sure all optimizations support empty pipelines
            class Empty : public Base {
                string inputPipeJson() { return "[]"; }
                string shardPipeJson() { return "[]"; }
                string mergePipeJson() { return "[]"; }
            };

            namespace moveFinalUnwindFromShardsToMerger {

                class OneUnwind : public Base {
                    string inputPipeJson() { return "[{$unwind: '$a'}]}"; }
                    string shardPipeJson() { return "[]}"; }
                    string mergePipeJson() { return "[{$unwind: '$a'}]}"; }
                };

                class TwoUnwind : public Base {
                    string inputPipeJson() { return "[{$unwind: '$a'}, {$unwind: '$b'}]}"; }
                    string shardPipeJson() { return "[]}"; }
                    string mergePipeJson() { return "[{$unwind: '$a'}, {$unwind: '$b'}]}"; }
                };

                class UnwindNotFinal : public Base {
                    string inputPipeJson() { return "[{$unwind: '$a'}, {$match: {a:1}}]}"; }
                    string shardPipeJson() { return "[{$unwind: '$a'}, {$match: {a:1}}]}"; }
                    string mergePipeJson() { return "[]}"; }
                };

                class UnwindWithOther : public Base {
                    string inputPipeJson() { return "[{$match: {a:1}}, {$unwind: '$a'}]}"; }
                    string shardPipeJson() { return "[{$match: {a:1}}]}"; }
                    string mergePipeJson() { return "[{$unwind: '$a'}]}"; }
                };
            } // namespace moveFinalUnwindFromShardsToMerger
        } // namespace Sharded
    } // namespace Optimizations

    class All : public Suite {
    public:
        All() : Suite( "pipeline" ) {
        }
        void setupTests() {
            add<FieldPath::Empty>();
            add<FieldPath::EmptyVector>();
            add<FieldPath::Simple>();
            add<FieldPath::SimpleVector>();
            add<FieldPath::DollarSign>();
            add<FieldPath::DollarSignPrefix>();
            add<FieldPath::Dotted>();
            add<FieldPath::VectorWithDot>();
            add<FieldPath::TwoFieldVector>();
            add<FieldPath::DollarSignPrefixSecondField>();
            add<FieldPath::TwoDotted>();
            add<FieldPath::TerminalDot>();
            add<FieldPath::PrefixDot>();
            add<FieldPath::AdjacentDots>();
            add<FieldPath::LetterBetweenDots>();
            add<FieldPath::NullCharacter>();
            add<FieldPath::VectorNullCharacter>();
            add<FieldPath::Tail>();
            add<FieldPath::TailThreeFields>();

            add<Optimizations::Sharded::Empty>();
            add<Optimizations::Sharded::moveFinalUnwindFromShardsToMerger::OneUnwind>();
            add<Optimizations::Sharded::moveFinalUnwindFromShardsToMerger::TwoUnwind>();
            add<Optimizations::Sharded::moveFinalUnwindFromShardsToMerger::UnwindNotFinal>();
            add<Optimizations::Sharded::moveFinalUnwindFromShardsToMerger::UnwindWithOther>();
        }
    } myall;
    
} // namespace PipelineTests
