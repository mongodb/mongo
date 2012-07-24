// documenttests.cpp : Unit tests for Document, Value, and related classes.

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

#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/value.h"

#include "dbtests.h"

namespace DocumentTests {

    namespace Document {

        using mongo::Document;

        BSONObj toBson( const intrusive_ptr<Document>& document ) {
            BSONObjBuilder bob;
            document->toBson( &bob );
            return bob.obj();
        }

        intrusive_ptr<Document> fromBson( BSONObj obj ) {
            return Document::createFromBsonObj( &obj );
        }

        void assertRoundTrips( const intrusive_ptr<Document>& document1 ) {
            BSONObj obj1 = toBson( document1 );
            intrusive_ptr<Document> document2 = fromBson( obj1 );
            BSONObj obj2 = toBson( document2 );
            ASSERT_EQUALS( obj1, obj2 );
            ASSERT_EQUALS( 0, Document::compare( document1, document2 ) );
        }

        /** Create a Document. */
        class Create {
        public:
            void run() {
                intrusive_ptr<Document> document = Document::create();
                ASSERT_EQUALS( 0U, document->getFieldCount() );
                assertRoundTrips( document );
            }
        };

        /** Create a Document from a BSONObj. */
        class CreateFromBsonObj {
        public:
            void run() {
                intrusive_ptr<Document> document = fromBson( BSONObj() );
                ASSERT_EQUALS( 0U, document->getFieldCount() );
                document = fromBson( BSON( "a" << 1 << "b" << "q" ) );
                ASSERT_EQUALS( 2U, document->getFieldCount() );
                ASSERT_EQUALS( "a", document->getField( 0 ).first );
                ASSERT_EQUALS( 1, document->getField( 0 ).second->getInt() );
                ASSERT_EQUALS( "b", document->getField( 1 ).first );
                ASSERT_EQUALS( "q", document->getField( 1 ).second->getString() );
                assertRoundTrips( document );
            }            
        };

        /** Add Document fields. */
        class AddField {
        public:
            void run() {
                intrusive_ptr<Document> document = Document::create();
                document->addField( "foo", Value::createInt( 1 ) );
                ASSERT_EQUALS( 1U, document->getFieldCount() );
                ASSERT_EQUALS( 1, document->getValue( "foo" )->getInt() );
                document->addField( "bar", Value::createInt( 99 ) );
                ASSERT_EQUALS( 2U, document->getFieldCount() );
                ASSERT_EQUALS( 99, document->getValue( "bar" )->getInt() );
                // No assertion is triggered by a duplicate field name.
                document->addField( "a", Value::createInt( 5 ) );
                ASSERT_EQUALS( 3U, document->getFieldCount() );
                assertRoundTrips( document );
            }
        };

        /** Get Document values. */
        class GetValue {
        public:
            void run() {
                intrusive_ptr<Document> document = fromBson( BSON( "a" << 1 << "b" << 2.2 ) );
                ASSERT_EQUALS( 1, document->getValue( "a" )->getInt() );
                ASSERT_EQUALS( 1, document->getField( "a" )->getInt() );
                ASSERT_EQUALS( 2.2, document->getValue( "b" )->getDouble() );
                ASSERT_EQUALS( 2.2, document->getField( "b" )->getDouble() );
                // Missing field.
                ASSERT( !document->getValue( "c" ) );
                ASSERT( !document->getField( "c" ) );
                assertRoundTrips( document );
            }
        };

        /** Get Document fields. */
        class SetField {
        public:
            void run() {
                intrusive_ptr<Document> document =
                        fromBson( BSON( "a" << 1 << "b" << 2.2 << "c" << 99 ) );
                // Set the first field.
                document->setField( 0, "new", Value::createString( "foo" ) );
                ASSERT_EQUALS( 3U, document->getFieldCount() );
                ASSERT( !document->getValue( "a" ) );
                ASSERT_EQUALS( "foo", document->getValue( "new" )->getString() );
                ASSERT_EQUALS( "new", document->getField( 0 ).first );
                ASSERT_EQUALS( "foo", document->getField( 0 ).second->getString() );
                assertRoundTrips( document );
                // Set the second field.
                document->setField( 1, "newer", Value::createString( "bar" ) );
                ASSERT_EQUALS( 3U, document->getFieldCount() );
                ASSERT( !document->getValue( "b" ) );
                ASSERT_EQUALS( "bar", document->getValue( "newer" )->getString() );
                ASSERT_EQUALS( "newer", document->getField( 1 ).first );
                ASSERT_EQUALS( "bar", document->getField( 1 ).second->getString() );
                assertRoundTrips( document );
                // Remove the second field.
                document->setField( 1, "n/a", NULL );
                ASSERT_EQUALS( 2U, document->getFieldCount() );
                ASSERT( !document->getValue( "newer" ) );
                ASSERT_EQUALS( "c", document->getField( 1 ).first );
                ASSERT_EQUALS( 99, document->getField( 1 ).second->getInt() );
                assertRoundTrips( document );
                // Remove the first field.
                document->setField( 0, "n/a", NULL );
                ASSERT_EQUALS( 1U, document->getFieldCount() );
                ASSERT( !document->getValue( "new" ) );
                ASSERT_EQUALS( "c", document->getField( 0 ).first );
                ASSERT_EQUALS( 99, document->getField( 0 ).second->getInt() );
                assertRoundTrips( document );
            }
        };

        /** Get Document field indexes. */
        class GetFieldIndex {
        public:
            void run() {
                intrusive_ptr<Document> document =
                        fromBson( BSON( "a" << 1 << "b" << 2.2 << "c" << 99 ) );
                ASSERT_EQUALS( 0U, document->getFieldIndex( "a" ) );
                ASSERT_EQUALS( 2U, document->getFieldIndex( "c" ) );
                ASSERT_EQUALS( 3U, document->getFieldIndex( "missing" ) );
                assertRoundTrips( document );
            }
        };

        /** Document comparator. */
        class Compare {
        public:
            void run() {
                assertComparison( 0, BSONObj(), BSONObj() );
                assertComparison( 0, BSON( "a" << 1 ), BSON( "a" << 1 ) );
                assertComparison( -1, BSONObj(), BSON( "a" << 1 ) );
                assertComparison( -1, BSON( "a" << 1 ), BSON( "c" << 1 ) );
                assertComparison( 0, BSON( "a" << 1 << "r" << 2 ), BSON( "a" << 1 << "r" << 2 ) );
                assertComparison( -1, BSON( "a" << 1 ), BSON( "a" << 1 << "r" << 2 ) );
                assertComparison( 0, BSON( "a" << 2 ), BSON( "a" << 2 ) );
                assertComparison( -1, BSON( "a" << 1 ), BSON( "a" << 2 ) );
                assertComparison( -1, BSON( "a" << 1 << "b" << 1 ), BSON( "a" << 1 << "b" << 2 ) );
                ASSERT_THROWS( assertComparison( 0, BSON( "a" << 1 ), BSON( "a" << "foo" ) ),
                               UserException );
            }
        public:
            int cmp( const BSONObj& a, const BSONObj& b ) {
                int result = Document::compare( fromBson( a ), fromBson( b ) );
                return // sign
                    result < 0 ? -1 :
                    result > 0 ? 1 :
                    0;
            }
            void assertComparison( int expectedResult, const BSONObj& a, const BSONObj& b ) {
                ASSERT_EQUALS( expectedResult, cmp( a, b ) );
                ASSERT_EQUALS( -expectedResult, cmp( b, a ) );
                if ( expectedResult == 0 ) {
                    ASSERT_EQUALS( hash( a ), hash( b ) );
                }
            }
            size_t hash( const BSONObj& obj ) {
                size_t seed = 0x106e1e1;
                fromBson( obj )->hash_combine( seed );
                return seed;
            }
        };

        /** Comparison based on a null field's name.  Differs from BSONObj comparison behavior. */
        class CompareNamedNull {
        public:
            void run() {
                BSONObj obj1 = BSON( "z" << BSONNULL );
                BSONObj obj2 = BSON( "a" << 1 );
                // Comparsion with type precedence.
                ASSERT( obj1.woCompare( obj2 ) < 0 );
                // Comparison with field name precedence.
                ASSERT( Document::compare( fromBson( obj1 ), fromBson( obj2 ) ) > 0 );
            }
        };

        /** Shallow copy clone of a single field Document. */
        class Clone {
        public:
            void run() {
                intrusive_ptr<Document> document = fromBson( BSON( "a" << BSON( "b" << 1 ) ) );
                intrusive_ptr<Document> clonedDocument = document->clone();
                // Check equality.
                ASSERT_EQUALS( 0, Document::compare( document, clonedDocument ) );
                // Check pointer equality of sub document.
                ASSERT_EQUALS( document->getValue( "a" )->getDocument(),
                               clonedDocument->getValue( "a" )->getDocument() );
                // Rename field in clone and ensure the original document's field is unchanged.
                clonedDocument->setField( 0, "renamed", clonedDocument->getValue( "a" ) );
                ASSERT_EQUALS( "a", document->getField( 0 ).first );
                // Drop the field in the clone and ensure the original document is unchanged.
                clonedDocument->setField( 0, "renamed", NULL );
                ASSERT_EQUALS( 0U, clonedDocument->getFieldCount() );
                ASSERT_EQUALS( BSON( "a" << BSON( "b" << 1 ) ), toBson( document ) );
            }
        };

        /** Shallow copy clone of a multi field Document. */
        class CloneMultipleFields {
        public:
            void run() {
                intrusive_ptr<Document> document =
                        fromBson( fromjson( "{a:1,b:['ra',4],c:{z:1},d:'lal'}" ) );
                intrusive_ptr<Document> clonedDocument = document->clone();
                ASSERT_EQUALS( 0, Document::compare( document, clonedDocument ) );
            }
        };

        /** FieldIterator for an empty Document. */
        class FieldIteratorEmpty {
        public:
            void run() {
                scoped_ptr<FieldIterator> iterator( Document::create()->createFieldIterator() );
                ASSERT( !iterator->more() );
            }
        };

        /** FieldIterator for a single field Document. */
        class FieldIteratorSingle {
        public:
            void run() {
                scoped_ptr<FieldIterator> iterator
                        ( fromBson( BSON( "a" << 1 ) )->createFieldIterator() );
                ASSERT( iterator->more() );
                Document::FieldPair field = iterator->next();
                ASSERT_EQUALS( "a", field.first );
                ASSERT_EQUALS( 1, field.second->getInt() );
                ASSERT( !iterator->more() );
            }
        };
        
        /** FieldIterator for a multiple field Document. */
        class FieldIteratorMultiple {
        public:
            void run() {
                scoped_ptr<FieldIterator> iterator
                        ( fromBson( BSON( "a" << 1 << "b" << 5.6 << "c" << "z" ) )->
                        createFieldIterator() );
                ASSERT( iterator->more() );
                Document::FieldPair field = iterator->next();
                ASSERT_EQUALS( "a", field.first );
                ASSERT_EQUALS( 1, field.second->getInt() );
                ASSERT( iterator->more() );
                field = iterator->next();
                ASSERT_EQUALS( "b", field.first );
                ASSERT_EQUALS( 5.6, field.second->getDouble() );
                ASSERT( iterator->more() );
                field = iterator->next();
                ASSERT_EQUALS( "c", field.first );
                ASSERT_EQUALS( "z", field.second->getString() );
                ASSERT( !iterator->more() );
            }
        };
        
    } // namespace Document

    namespace Value {

        using mongo::Value;

        BSONObj toBson( const intrusive_ptr<const Value>& value ) {
            BSONObjBuilder bob;
            value->addToBsonObj( &bob, "" );
            return bob.obj();
        }

        intrusive_ptr<const Value> fromBson( const BSONObj& obj ) {
            BSONElement element = obj.firstElement();
            return Value::createFromBsonElement( &element );
        }

        void assertRoundTrips( const intrusive_ptr<const Value>& value1 ) {
            BSONObj obj1 = toBson( value1 );
            intrusive_ptr<const Value> value2 = fromBson( obj1 );
            BSONObj obj2 = toBson( value2 );
            ASSERT_EQUALS( obj1, obj2 );
            ASSERT( value1 == value2 );
        }

        /** Int type. */
        class Int {
        public:
            void run() {
                intrusive_ptr<const Value> value = Value::createInt( 5 );
                ASSERT_EQUALS( 5, value->getInt() );
                ASSERT_EQUALS( 5, value->getLong() );
                ASSERT_EQUALS( 5, value->getDouble() );
                ASSERT_EQUALS( NumberInt, value->getType() );
                assertRoundTrips( value );
            }
        };

        /** Long type. */
        class Long {
        public:
            void run() {
                intrusive_ptr<const Value> value = Value::createLong( 99 );
                ASSERT_EQUALS( 99, value->getLong() );
                ASSERT_EQUALS( 99, value->getDouble() );
                ASSERT_EQUALS( NumberLong, value->getType() );
                assertRoundTrips( value );
            }
        };
        
        /** Double type. */
        class Double {
        public:
            void run() {
                intrusive_ptr<const Value> value = Value::createDouble( 5.5 );
                ASSERT_EQUALS( 5.5, value->getDouble() );
                ASSERT_EQUALS( NumberDouble, value->getType() );
                assertRoundTrips( value );
            }
        };

        /** String type. */
        class String {
        public:
            void run() {
                intrusive_ptr<const Value> value = Value::createString( "foo" );
                ASSERT_EQUALS( "foo", value->getString() );
                ASSERT_EQUALS( mongo::String, value->getType() );
                assertRoundTrips( value );
            }
        };

        /** String with a null character. */
        class StringWithNull {
        public:
            void run() {
                string withNull( "a\0b", 3 );
                BSONObj objWithNull = BSON( "" << withNull );
                ASSERT_EQUALS( withNull, objWithNull[ "" ].str() );
                intrusive_ptr<const Value> value = fromBson( objWithNull );
                ASSERT_EQUALS( withNull, value->getString() );
                assertRoundTrips( value );                
            }
        };

        /** Date type. */
        class Date {
        public:
            void run() {
                intrusive_ptr<const Value> value = Value::createDate( Date_t( 999 ) );
                ASSERT_EQUALS( Date_t( 999 ), value->getDate() );
                ASSERT_EQUALS( mongo::Date, value->getType() );
                assertRoundTrips( value );
            }
        };
        
        /** Timestamp type. */
        class Timestamp {
        public:
            void run() {
                intrusive_ptr<const Value> value = Value::createTimestamp( OpTime( 777 ) );
                ASSERT( OpTime( 777 ) == value->getTimestamp() );
                ASSERT_EQUALS( mongo::Timestamp, value->getType() );
                assertRoundTrips( value );
            }
        };

        /** Document with no fields. */
        class EmptyDocument {
        public:
            void run() {
                intrusive_ptr<mongo::Document> document = mongo::Document::create();
                intrusive_ptr<const Value> value = Value::createDocument( document );
                ASSERT_EQUALS( document, value->getDocument() );
                ASSERT_EQUALS( Object, value->getType() );                
                assertRoundTrips( value );
            }
        };

        /** Document type. */
        class Document {
        public:
            void run() {
                intrusive_ptr<mongo::Document> document = mongo::Document::create();
                document->addField( "a", Value::createInt( 5 ) );
                document->addField( "apple", Value::createString( "rrr" ) );
                document->addField( "banana", Value::createDouble( -.3 ) );
                intrusive_ptr<const Value> value = Value::createDocument( document );
                // Check document pointers are equal.
                ASSERT_EQUALS( document, value->getDocument() );
                // Check document contents.
                ASSERT_EQUALS( 5, document->getValue( "a" )->getInt() );
                ASSERT_EQUALS( "rrr", document->getValue( "apple" )->getString() );
                ASSERT_EQUALS( -.3, document->getValue( "banana" )->getDouble() );
                ASSERT_EQUALS( Object, value->getType() );                
                assertRoundTrips( value );
            }
        };
        
        /** Array with no elements. */
        class EmptyArray {
        public:
            void run() {
                vector<intrusive_ptr<const Value> > array;
                intrusive_ptr<const Value> value = Value::createArray( array );
                intrusive_ptr<ValueIterator> arrayIterator = value->getArray();
                ASSERT( !arrayIterator->more() );
                ASSERT_EQUALS( Array, value->getType() );
                ASSERT_EQUALS( 0U, value->getArrayLength() );
                assertRoundTrips( value );
            }
        };

        /** Array type. */
        class Array {
        public:
            void run() {
                vector<intrusive_ptr<const Value> > array;
                array.push_back( Value::createInt( 5 ) );
                array.push_back( Value::createString( "lala" ) );
                array.push_back( Value::createDouble( 3.14 ) );
                intrusive_ptr<const Value> value = Value::createArray( array );
                intrusive_ptr<ValueIterator> arrayIterator = value->getArray();
                ASSERT( arrayIterator->more() );
                ASSERT_EQUALS( 5, arrayIterator->next()->getInt() );
                ASSERT_EQUALS( "lala", arrayIterator->next()->getString() );
                ASSERT_EQUALS( 3.14, arrayIterator->next()->getDouble() );
                ASSERT( !arrayIterator->more() );
                ASSERT_EQUALS( mongo::Array, value->getType() );
                ASSERT_EQUALS( 3U, value->getArrayLength() );
                assertRoundTrips( value );
            }
        };

        /** Oid type. */
        class Oid {
        public:
            void run() {
                intrusive_ptr<const Value> value =
                        fromBson( BSON( "" << OID( "abcdefabcdefabcdefabcdef" ) ) );
                ASSERT_EQUALS( OID( "abcdefabcdefabcdefabcdef" ), value->getOid() );
                ASSERT_EQUALS( jstOID, value->getType() );
                assertRoundTrips( value );
            }
        };

        /** Bool type. */
        class Bool {
        public:
            void run() {
                intrusive_ptr<const Value> value = fromBson( BSON( "" << true ) );
                ASSERT_EQUALS( true, value->getBool() );
                ASSERT_EQUALS( mongo::Bool, value->getType() );
                assertRoundTrips( value );                
            }
        };

        /** Regex type. */
        class Regex {
        public:
            void run() {
                intrusive_ptr<const Value> value = fromBson( fromjson( "{'':/abc/}" ) );
                ASSERT_EQUALS( "abc", value->getRegex() );
                ASSERT_EQUALS( RegEx, value->getType() );
                if ( 0 ) { // SERVER-6470
                assertRoundTrips( value );
                }
            }
        };

        /** Symbol type (currently unsupported). */
        class Symbol {
        public:
            void run() {
                BSONObjBuilder bob;
                bob.appendSymbol( "", "FOOBAR" );
                intrusive_ptr<const Value> value = fromBson( bob.obj() );
                ASSERT_EQUALS( "FOOBAR", value->getSymbol() );
                ASSERT_EQUALS( mongo::Symbol, value->getType() );
                assertRoundTrips( value );
            }
        };

        /** Undefined type. */
        class Undefined {
        public:
            void run() {
                intrusive_ptr<const Value> value = Value::getUndefined();
                ASSERT_EQUALS( mongo::Undefined, value->getType() );
                assertRoundTrips( value );
            }
        };

        /** Null type. */
        class Null {
        public:
            void run() {
                intrusive_ptr<const Value> value = Value::getNull();
                ASSERT_EQUALS( jstNULL, value->getType() );
                assertRoundTrips( value );
            }
        };

        /** True value. */
        class True {
        public:
            void run() {
                intrusive_ptr<const Value> value = Value::getTrue();
                ASSERT_EQUALS( true, value->getBool() );
                ASSERT_EQUALS( mongo::Bool, value->getType() );
                assertRoundTrips( value );
            }            
        };

        /** False value. */
        class False {
        public:
            void run() {
                intrusive_ptr<const Value> value = Value::getFalse();
                ASSERT_EQUALS( false, value->getBool() );
                ASSERT_EQUALS( mongo::Bool, value->getType() );
                assertRoundTrips( value );
            }            
        };
        
        /** -1 value. */
        class MinusOne {
        public:
            void run() {
                intrusive_ptr<const Value> value = Value::getMinusOne();
                ASSERT_EQUALS( -1, value->getInt() );
                ASSERT_EQUALS( NumberInt, value->getType() );
                assertRoundTrips( value );
            }
        };
        
        /** 0 value. */
        class Zero {
        public:
            void run() {
                intrusive_ptr<const Value> value = Value::getZero();
                ASSERT_EQUALS( 0, value->getInt() );
                ASSERT_EQUALS( NumberInt, value->getType() );
                assertRoundTrips( value );
            }
        };
        
        /** 1 value. */
        class One {
        public:
            void run() {
                intrusive_ptr<const Value> value = Value::getOne();
                ASSERT_EQUALS( 1, value->getInt() );
                ASSERT_EQUALS( NumberInt, value->getType() );
                assertRoundTrips( value );
            }
        };

        namespace Coerce {

            class ToBoolBase {
            public:
                virtual ~ToBoolBase() {
                }
                void run() {
                    ASSERT_EQUALS( expected(), value()->coerceToBool() );
                }
            protected:
                virtual intrusive_ptr<const Value> value() = 0;
                virtual bool expected() = 0;
            };

            class ToBoolTrue : public ToBoolBase {
                bool expected() { return true; }
            };
            
            class ToBoolFalse : public ToBoolBase {
                bool expected() { return false; }
            };

            /** Coerce 0 to bool. */
            class ZeroIntToBool : public ToBoolFalse {
                intrusive_ptr<const Value> value() { return Value::createInt( 0 ); }
            };
            
            /** Coerce -1 to bool. */
            class NonZeroIntToBool : public ToBoolTrue {
                intrusive_ptr<const Value> value() { return Value::createInt( -1 ); }
            };
            
            /** Coerce 0LL to bool. */
            class ZeroLongToBool : public ToBoolFalse {
                intrusive_ptr<const Value> value() { return Value::createLong( 0 ); }
            };
            
            /** Coerce 5LL to bool. */
            class NonZeroLongToBool : public ToBoolTrue {
                intrusive_ptr<const Value> value() { return Value::createLong( 5 ); }
            };
            
            /** Coerce 0.0 to bool. */
            class ZeroDoubleToBool : public ToBoolFalse {
                intrusive_ptr<const Value> value() { return Value::createDouble( 0 ); }
            };
            
            /** Coerce -1.3 to bool. */
            class NonZeroDoubleToBool : public ToBoolTrue {
                intrusive_ptr<const Value> value() { return Value::createDouble( -1.3 ); }
            };

            /** Coerce "" to bool. */
            class StringToBool : public ToBoolTrue {
                intrusive_ptr<const Value> value() { return Value::createString( "" ); }                
            };
            
            /** Coerce {} to bool. */
            class ObjectToBool : public ToBoolTrue {
                intrusive_ptr<const Value> value() {
                    return Value::createDocument( mongo::Document::create() );
                }
            };
            
            /** Coerce [] to bool. */
            class ArrayToBool : public ToBoolTrue {
                intrusive_ptr<const Value> value() {
                    return Value::createArray( vector<intrusive_ptr<const Value> >() );
                }
            };

            /** Coerce Date_t() to bool. */
            class DateToBool : public ToBoolTrue {
                intrusive_ptr<const Value> value() { return Value::createDate( Date_t() ); }
            };
            
            /** Coerce // to bool. */
            class RegexToBool : public ToBoolTrue {
                intrusive_ptr<const Value> value() { return fromBson( fromjson( "{''://}" ) ); }
            };
            
            /** Coerce true to bool. */
            class TrueToBool : public ToBoolTrue {
                intrusive_ptr<const Value> value() { return fromBson( BSON( "" << true ) ); }
            };
            
            /** Coerce false to bool. */
            class FalseToBool : public ToBoolFalse {
                intrusive_ptr<const Value> value() { return fromBson( BSON( "" << false ) ); }
            };
            
            /** Coerce null to bool. */
            class NullToBool : public ToBoolFalse {
                intrusive_ptr<const Value> value() { return Value::getNull(); }
            };
            
            /** Coerce undefined to bool. */
            class UndefinedToBool : public ToBoolFalse {
                intrusive_ptr<const Value> value() { return Value::getUndefined(); }
            };

            class ToIntBase {
            public:
                virtual ~ToIntBase() {
                }
                void run() {
                    ASSERT_EQUALS( expected(), value()->coerceToInt() );
                }
            protected:
                virtual intrusive_ptr<const Value> value() = 0;
                virtual int expected() { return 0; }
            };

            /** Coerce -5 to int. */
            class IntToInt : public ToIntBase {
                intrusive_ptr<const Value> value() { return Value::createInt( -5 ); }
                int expected() { return -5; }
            };
            
            /** Coerce long to int. */
            class LongToInt : public ToIntBase {
                intrusive_ptr<const Value> value() { return Value::createLong( 0xff00000007LL ); }
                int expected() { return 7; }
            };
            
            /** Coerce 9.8 to int. */
            class DoubleToInt : public ToIntBase {
                intrusive_ptr<const Value> value() { return Value::createDouble( 9.8 ); }
                int expected() { return 9; }
            };
            
            /** Coerce null to int. */
            class NullToInt : public ToIntBase {
                intrusive_ptr<const Value> value() { return Value::getNull(); }
            };
            
            /** Coerce undefined to int. */
            class UndefinedToInt : public ToIntBase {
                intrusive_ptr<const Value> value() { return Value::getUndefined(); }
            };
            
            /** Coerce "" to int unsupported. */
            class StringToInt {
            public:
                void run() {
                    ASSERT_THROWS( Value::createString( "" )->coerceToInt(), UserException );
                }
            };
            
            class ToLongBase {
            public:
                virtual ~ToLongBase() {
                }
                void run() {
                    ASSERT_EQUALS( expected(), value()->coerceToLong() );
                }
            protected:
                virtual intrusive_ptr<const Value> value() = 0;
                virtual long long expected() { return 0; }
            };
            
            /** Coerce -5 to long. */
            class IntToLong : public ToLongBase {
                intrusive_ptr<const Value> value() { return Value::createInt( -5 ); }
                long long expected() { return -5; }
            };
            
            /** Coerce long to long. */
            class LongToLong : public ToLongBase {
                intrusive_ptr<const Value> value() { return Value::createLong( 0xff00000007LL ); }
                long long expected() { return 0xff00000007LL; }
            };
            
            /** Coerce 9.8 to long. */
            class DoubleToLong : public ToLongBase {
                intrusive_ptr<const Value> value() { return Value::createDouble( 9.8 ); }
                long long expected() { return 9; }
            };
            
            /** Coerce null to long. */
            class NullToLong : public ToLongBase {
                intrusive_ptr<const Value> value() { return Value::getNull(); }
            };
            
            /** Coerce undefined to long. */
            class UndefinedToLong : public ToLongBase {
                intrusive_ptr<const Value> value() { return Value::getUndefined(); }
            };
            
            /** Coerce string to long unsupported. */
            class StringToLong {
            public:
                void run() {
                    ASSERT_THROWS( Value::createString( "" )->coerceToLong(), UserException );
                }
            };
            
            class ToDoubleBase {
            public:
                virtual ~ToDoubleBase() {
                }
                void run() {
                    ASSERT_EQUALS( expected(), value()->coerceToDouble() );
                }
            protected:
                virtual intrusive_ptr<const Value> value() = 0;
                virtual double expected() { return 0; }
            };
            
            /** Coerce -5 to double. */
            class IntToDouble : public ToDoubleBase {
                intrusive_ptr<const Value> value() { return Value::createInt( -5 ); }
                double expected() { return -5; }
            };
            
            /** Coerce long to double. */
            class LongToDouble : public ToDoubleBase {
                intrusive_ptr<const Value> value() {
                    // A long that cannot be exactly represented as a double.
                    return Value::createDouble( 0x8fffffffffffffffLL );
                }
                double expected() { return (double)0x8fffffffffffffffLL; }
            };
            
            /** Coerce double to double. */
            class DoubleToDouble : public ToDoubleBase {
                intrusive_ptr<const Value> value() { return Value::createDouble( 9.8 ); }
                double expected() { return 9.8; }
            };
            
            /** Coerce null to double. */
            class NullToDouble : public ToDoubleBase {
                intrusive_ptr<const Value> value() { return Value::getNull(); }
            };
            
            /** Coerce undefined to double. */
            class UndefinedToDouble : public ToDoubleBase {
                intrusive_ptr<const Value> value() { return Value::getUndefined(); }
            };
            
            /** Coerce string to double unsupported. */
            class StringToDouble {
            public:
                void run() {
                    ASSERT_THROWS( Value::createString( "" )->coerceToDouble(), UserException );
                }
            };

            class ToDateBase {
            public:
                virtual ~ToDateBase() {
                }
                void run() {
                    ASSERT_EQUALS( expected(), value()->coerceToDate() );
                }
            protected:
                virtual intrusive_ptr<const Value> value() = 0;
                virtual Date_t expected() = 0;
            };

            /** Coerce date to date. */
            class DateToDate : public ToDateBase {
                intrusive_ptr<const Value> value() { return Value::createDate( Date_t( 888 ) ); }
                Date_t expected() { return Date_t( 888 ); }
            };

            /**
             * Convert timestamp to date.  This extracts the time portion of the timestamp, which
             * is different from BSON behavior of interpreting all bytes as a date.
             */
            class TimestampToDate : public ToDateBase {
                intrusive_ptr<const Value> value() {
                    return Value::createTimestamp( OpTime( 777, 666 ) );
                }
                Date_t expected() { return Date_t( 777 * 1000 ); }
            };
            
            /** Coerce string to date unsupported. */
            class StringToDate {
            public:
                void run() {
                    ASSERT_THROWS( Value::createString( "" )->coerceToDate(), UserException );
                }
            };
            
            class ToStringBase {
            public:
                virtual ~ToStringBase() {
                }
                void run() {
                    ASSERT_EQUALS( expected(), value()->coerceToString() );
                }
            protected:
                virtual intrusive_ptr<const Value> value() = 0;
                virtual string expected() { return ""; }
            };

            /** Coerce -0.2 to string. */
            class DoubleToString : public ToStringBase {
                intrusive_ptr<const Value> value() { return Value::createDouble( -0.2 ); }
                string expected() { return "-0.2"; }
            };
            
            /** Coerce -4 to string. */
            class IntToString : public ToStringBase {
                intrusive_ptr<const Value> value() { return Value::createInt( -4 ); }
                string expected() { return "-4"; }
            };
            
            /** Coerce 10000LL to string. */
            class LongToString : public ToStringBase {
                intrusive_ptr<const Value> value() { return Value::createLong( 10000 ); }
                string expected() { return "10000"; }
            };
            
            /** Coerce string to string. */
            class StringToString : public ToStringBase {
                intrusive_ptr<const Value> value() { return Value::createString( "fO_o" ); }
                string expected() { return "fO_o"; }
            };
            
            /** Coerce timestamp to string. */
            class TimestampToString : public ToStringBase {
                intrusive_ptr<const Value> value() {
                    return Value::createTimestamp( OpTime( 1, 2 ) );
                }
                string expected() { return OpTime( 1, 2 ).toStringPretty(); }
            };
            
            /** Coerce date to string. */
            class DateToString : public ToStringBase {
                intrusive_ptr<const Value> value() { return Value::createDate( Date_t( 12345 ) ); }
                string expected() { return Date_t( 12345 ).toString(); }
            };

            /** Coerce null to string. */
            class NullToString : public ToStringBase {
                intrusive_ptr<const Value> value() { return Value::getNull(); }
            };

            /** Coerce undefined to string. */
            class UndefinedToString : public ToStringBase {
                intrusive_ptr<const Value> value() { return Value::getUndefined(); }
            };

            /** Coerce document to string unsupported. */
            class DocumentToString {
            public:
                void run() {
                    ASSERT_THROWS( Value::createDocument
                                        ( mongo::Document::create() )->coerceToString(),
                                   UserException );
                }
            };

            /** Coerce timestamp to timestamp. */
            class TimestampToTimestamp {
            public:
                void run() {
                    intrusive_ptr<const Value> value = Value::createTimestamp( OpTime( 1010 ) );
                    ASSERT( OpTime( 1010 ) == value->coerceToTimestamp() );
                }
            };

            /** Coerce date to timestamp unsupported. */
            class DateToTimestamp {
            public:
                void run() {
                    ASSERT_THROWS( Value::createDate( Date_t( 1010 ) )->coerceToTimestamp(),
                                   UserException );
                }
            };

        } // namespace Coerce

        /** Get the "widest" of two numeric types. */
        class GetWidestNumeric {
        public:
            void run() {
                using mongo::Undefined;
                
                // Numeric types.
                assertWidest( NumberInt, NumberInt, NumberInt );
                assertWidest( NumberLong, NumberInt, NumberLong );
                assertWidest( NumberDouble, NumberInt, NumberDouble );
                assertWidest( NumberLong, NumberLong, NumberLong );
                assertWidest( NumberDouble, NumberLong, NumberDouble );
                assertWidest( NumberDouble, NumberDouble, NumberDouble );
                
                // Missing value and numeric types.
                assertWidest( NumberInt, NumberInt, jstNULL );
                assertWidest( NumberInt, NumberInt, Undefined );
                assertWidest( NumberLong, NumberLong, jstNULL );
                assertWidest( NumberLong, NumberLong, Undefined );
                assertWidest( NumberDouble, NumberDouble, jstNULL );
                assertWidest( NumberDouble, NumberDouble, Undefined );

                // Missing value types (result Undefined).
                assertWidest( Undefined, jstNULL, jstNULL );
                assertWidest( Undefined, jstNULL, Undefined );
                assertWidest( Undefined, Undefined, Undefined );

                // Other types (result Undefined).
                assertWidest( Undefined, NumberInt, mongo::Bool );
                assertWidest( Undefined, mongo::String, NumberDouble );
            }
        private:
            void assertWidest( BSONType expectedWidest, BSONType a, BSONType b ) {
                ASSERT_EQUALS( expectedWidest, Value::getWidestNumeric( a, b ) );
                ASSERT_EQUALS( expectedWidest, Value::getWidestNumeric( b, a ) );
            }
        };

        /** Add a Value to a BSONObj. */
        class AddToBsonObj {
        public:
            void run() {
                BSONObjBuilder bob;
                Value::createDouble( 4.4 )->addToBsonObj( &bob, "a" );
                Value::createInt( 22 )->addToBsonObj( &bob, "b" );
                Value::createString( "astring" )->addToBsonObj( &bob, "c" );
                ASSERT_EQUALS( BSON( "a" << 4.4 << "b" << 22 << "c" << "astring" ), bob.obj() );
            }
        };
        
        /** Add a Value to a BSONArray. */
        class AddToBsonArray {
        public:
            void run() {
                BSONArrayBuilder bab;
                Value::createDouble( 4.4 )->addToBsonArray( &bab );
                Value::createInt( 22 )->addToBsonArray( &bab );
                Value::createString( "astring" )->addToBsonArray( &bab );
                ASSERT_EQUALS( BSON_ARRAY( 4.4 << 22 << "astring" ), bab.arr() );
            }
        };

        /** Value comparator. */
        class Compare {
        public:
            void run() {
                BSONObjBuilder undefinedBuilder;
                undefinedBuilder.appendUndefined( "" );
                BSONObj undefined = undefinedBuilder.obj();

                // Undefined / null.
                assertComparison( 0, undefined, undefined );
                assertComparison( -1, undefined, BSON( "" << BSONNULL ) );
                assertComparison( 0, BSON( "" << BSONNULL ), BSON( "" << BSONNULL ) );

                // Undefined / null with other types.
                assertComparison( -1, undefined, BSON( "" << 1 ) );
                assertComparison( -1, undefined, BSON( "" << "bar" ) );
                assertComparison( -1, BSON( "" << BSONNULL ), BSON( "" << -1 ) );
                assertComparison( -1, BSON( "" << BSONNULL ), BSON( "" << "bar" ) );

                // Numeric types.
                assertComparison( 0, 5, 5LL );
                assertComparison( 0, -2, -2.0 );
                assertComparison( 0, 90LL, 90.0 );
                assertComparison( -1, 5, 6LL );
                assertComparison( -1, -2, 2.1 );
                assertComparison( 1, 90LL, 89.999 );
                assertComparison( -1, 90, 90.1 );
                assertComparison( 0, numeric_limits<double>::quiet_NaN(),
                                  numeric_limits<double>::signaling_NaN() );
                if ( 0 ) { // SERVER-6126
                assertComparison( -1, numeric_limits<double>::quiet_NaN(), 5 );
                }

                // Otherwise comparing different types is not supported.
                ASSERT_THROWS( assertComparison( 0, 90, "abc" ), UserException );
                ASSERT_THROWS( assertComparison( 0, 90, BSON( "a" << "b" ) ), UserException );

                // String comparison.
                assertComparison( -1, "", "a" );
                assertComparison( 0, "a", "a" );
                assertComparison( -1, "a", "b" );
                assertComparison( -1, "aa", "b" );
                assertComparison( 1, "bb", "b" );
                assertComparison( 1, "bb", "b" );
                assertComparison( 1, "b-", "b" );
                assertComparison( -1, "b-", "ba" );
                if ( 0 ) {
                // With a null character.
                assertComparison( 1, string( "a\0", 2 ), "a" );
                }

                // Object.
                assertComparison( 0, fromjson( "{'':{}}" ), fromjson( "{'':{}}" ) );
                assertComparison( 0, fromjson( "{'':{x:1}}" ), fromjson( "{'':{x:1}}" ) );
                assertComparison( -1, fromjson( "{'':{}}" ), fromjson( "{'':{x:1}}" ) );

                // Array.
                assertComparison( 0, fromjson( "{'':[]}" ), fromjson( "{'':[]}" ) );
                assertComparison( -1, fromjson( "{'':[0]}" ), fromjson( "{'':[1]}" ) );
                assertComparison( -1, fromjson( "{'':[0,0]}" ), fromjson( "{'':[1]}" ) );
                assertComparison( -1, fromjson( "{'':[0]}" ), fromjson( "{'':[0,0]}" ) );
                // Assertion on nested type mismatch.
                ASSERT_THROWS( assertComparison( 0, fromjson( "{'':[0]}" ),
                                                 fromjson( "{'':['']}" ) ),
                               UserException );

                // OID.
                assertComparison( 0, OID( "abcdefabcdefabcdefabcdef" ),
                                  OID( "abcdefabcdefabcdefabcdef" ) );
                assertComparison( 1, OID( "abcdefabcdefabcdefabcdef" ),
                                  OID( "010101010101010101010101" ) );

                // Bool.
                assertComparison( 0, true, true );
                assertComparison( 0, false, false );
                assertComparison( 1, true, false );

                // Date.
                assertComparison( 0, Date_t( 555 ), Date_t( 555 ) );
                assertComparison( 1, Date_t( 555 ), Date_t( 554 ) );
                // Negative date.
                assertComparison( 1, Date_t( 0 ), Date_t( -1 ) );

                // Regex.
                assertComparison( 0, fromjson( "{'':/a/}" ), fromjson( "{'':/a/}" ) );
                assertComparison( 0, fromjson( "{'':/a/}" ),
                                  // Regex options are ignored.
                                  fromjson( "{'':/a/i}" ) );
                assertComparison( -1, fromjson( "{'':/a/}" ), fromjson( "{'':/aa/}" ) );

                // Timestamp.
                assertComparison( 0, OpTime( 1234 ), OpTime( 1234 ) );
                assertComparison( -1, OpTime( 4 ), OpTime( 1234 ) );
            }
        private:
            template<class T,class U>
            void assertComparison( int expectedResult, const T& a, const U& b ) {
                assertComparison( expectedResult, BSON( "" << a ), BSON( "" << b ) );
            }
            void assertComparison( int expectedResult, const OpTime& a, const OpTime& b ) {
                BSONObjBuilder first;
                first.appendTimestamp( "", a.asDate() );
                BSONObjBuilder second;
                second.appendTimestamp( "", b.asDate() );
                assertComparison( expectedResult, first.obj(), second.obj() );
            }
            int cmp( const BSONObj& a, const BSONObj& b ) {
                int result = Value::compare( fromBson( a ), fromBson( b ) );
                return // sign
                    result > 0 ? 1 :
                    result < 0 ? -1 :
                    0;
            }
            void assertComparison( int expectedResult, const BSONObj& a, const BSONObj& b ) {
                ASSERT_EQUALS( expectedResult, cmp( a, b ) );
                ASSERT_EQUALS( -expectedResult, cmp( b, a ) );
                if ( expectedResult == 0 ) {
                    // Equal values must hash equally.
                    ASSERT_EQUALS( hash( a ), hash( b ) );
                }
            }
            size_t hash( const BSONObj& obj ) {
                size_t seed = 0xf00ba6;
                fromBson( obj )->hash_combine( seed );
                return seed;
            }
        };
        
    } // namespace Value

    class All : public Suite {
    public:
        All() : Suite( "document" ) {
        }
        void setupTests() {
            add<Document::Create>();
            add<Document::CreateFromBsonObj>();
            add<Document::AddField>();
            add<Document::GetValue>();
            add<Document::SetField>();
            add<Document::GetFieldIndex>();
            add<Document::Compare>();
            add<Document::CompareNamedNull>();
            add<Document::Clone>();
            add<Document::CloneMultipleFields>();
            add<Document::FieldIteratorEmpty>();
            add<Document::FieldIteratorSingle>();
            add<Document::FieldIteratorMultiple>();

            add<Value::Int>();
            add<Value::Long>();
            add<Value::Double>();
            add<Value::String>();
            if ( 0 ) { // SERVER-6556
            add<Value::StringWithNull>();
            }
            add<Value::Date>();
            add<Value::Timestamp>();
            add<Value::EmptyDocument>();
            add<Value::EmptyArray>();
            add<Value::Array>();
            add<Value::Oid>();
            add<Value::Bool>();
            add<Value::Regex>();
            if ( 0 ) {
            add<Value::Symbol>();
            }
            add<Value::Undefined>();
            add<Value::Null>();
            add<Value::True>();
            add<Value::False>();
            add<Value::MinusOne>();
            add<Value::Zero>();
            add<Value::One>();

            add<Value::Coerce::ZeroIntToBool>();
            add<Value::Coerce::NonZeroIntToBool>();
            add<Value::Coerce::ZeroLongToBool>();
            add<Value::Coerce::NonZeroLongToBool>();
            add<Value::Coerce::ZeroDoubleToBool>();
            add<Value::Coerce::NonZeroDoubleToBool>();
            add<Value::Coerce::StringToBool>();
            add<Value::Coerce::ObjectToBool>();
            add<Value::Coerce::ArrayToBool>();
            add<Value::Coerce::DateToBool>();
            add<Value::Coerce::RegexToBool>();
            add<Value::Coerce::TrueToBool>();
            add<Value::Coerce::FalseToBool>();
            add<Value::Coerce::NullToBool>();
            add<Value::Coerce::UndefinedToBool>();
            add<Value::Coerce::IntToInt>();
            add<Value::Coerce::LongToInt>();
            add<Value::Coerce::DoubleToInt>();
            add<Value::Coerce::NullToInt>();
            add<Value::Coerce::UndefinedToInt>();
            add<Value::Coerce::StringToInt>();
            add<Value::Coerce::IntToLong>();
            add<Value::Coerce::LongToLong>();
            add<Value::Coerce::DoubleToLong>();
            add<Value::Coerce::NullToLong>();
            add<Value::Coerce::UndefinedToLong>();
            add<Value::Coerce::StringToLong>();
            add<Value::Coerce::IntToDouble>();
            add<Value::Coerce::LongToDouble>();
            add<Value::Coerce::DoubleToDouble>();
            add<Value::Coerce::NullToDouble>();
            add<Value::Coerce::UndefinedToDouble>();
            add<Value::Coerce::StringToDouble>();
            add<Value::Coerce::DateToDate>();
            add<Value::Coerce::TimestampToDate>();
            add<Value::Coerce::StringToDate>();
            add<Value::Coerce::DoubleToString>();
            add<Value::Coerce::IntToString>();
            add<Value::Coerce::LongToString>();
            add<Value::Coerce::StringToString>();
            add<Value::Coerce::TimestampToString>();
            add<Value::Coerce::DateToString>();
            add<Value::Coerce::NullToString>();
            add<Value::Coerce::UndefinedToString>();
            add<Value::Coerce::DocumentToString>();
            add<Value::Coerce::TimestampToTimestamp>();
            add<Value::Coerce::DateToTimestamp>();

            add<Value::GetWidestNumeric>();
            add<Value::AddToBsonObj>();
            add<Value::AddToBsonArray>();
            add<Value::Compare>();
        }
    } myall;
    
} // namespace DocumentTests
