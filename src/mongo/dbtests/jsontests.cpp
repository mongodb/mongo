// jsontests.cpp - Tests for json.{h,cpp} code and BSONObj::jsonString()
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

#include "mongo/pch.h"

#include <limits>

#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/dbtests/dbtests.h"


namespace JsonTests {
    namespace JsonStringTests {

        class Empty {
        public:
            void run() {
                ASSERT_EQUALS( "{}", BSONObj().jsonString( Strict ) );
            }
        };

        class SingleStringMember {
        public:
            void run() {
                ASSERT_EQUALS( "{ \"a\" : \"b\" }", BSON( "a" << "b" ).jsonString( Strict ) );
            }
        };

        class EscapedCharacters {
        public:
            void run() {
                BSONObjBuilder b;
                b.append( "a", "\" \\ / \b \f \n \r \t" );
                ASSERT_EQUALS( "{ \"a\" : \"\\\" \\\\ / \\b \\f \\n \\r \\t\" }", b.done().jsonString( Strict ) );
            }
        };

        // per http://www.ietf.org/rfc/rfc4627.txt, control characters are
        // (U+0000 through U+001F).  U+007F is not mentioned as a control character.
        class AdditionalControlCharacters {
        public:
            void run() {
                BSONObjBuilder b;
                b.append( "a", "\x1 \x1f" );
                ASSERT_EQUALS( "{ \"a\" : \"\\u0001 \\u001f\" }", b.done().jsonString( Strict ) );
            }
        };

        class ExtendedAscii {
        public:
            void run() {
                BSONObjBuilder b;
                b.append( "a", "\x80" );
                ASSERT_EQUALS( "{ \"a\" : \"\x80\" }", b.done().jsonString( Strict ) );
            }
        };

        class EscapeFieldName {
        public:
            void run() {
                BSONObjBuilder b;
                b.append( "\t", "b" );
                ASSERT_EQUALS( "{ \"\\t\" : \"b\" }", b.done().jsonString( Strict ) );
            }
        };

        class SingleIntMember {
        public:
            void run() {
                BSONObjBuilder b;
                b.append( "a", 1 );
                ASSERT_EQUALS( "{ \"a\" : 1 }", b.done().jsonString( Strict ) );
            }
        };

        class SingleNumberMember {
        public:
            void run() {
                BSONObjBuilder b;
                b.append( "a", 1.5 );
                ASSERT_EQUALS( "{ \"a\" : 1.5 }", b.done().jsonString( Strict ) );
            }
        };

        class InvalidNumbers {
        public:
            void run() {
                BSONObjBuilder c;
                c.append( "a", numeric_limits< double >::quiet_NaN() );
                string s = c.done().jsonString( Strict );
                // Note there is no NaN in the JSON RFC but what would be the alternative?
                ASSERT( str::contains(s, "NaN") );

                // commented out assertion as it doesn't throw anymore:
                //ASSERT_THROWS( c.done().jsonString( Strict ), AssertionException );

                BSONObjBuilder d;
                d.append( "a", numeric_limits< double >::signaling_NaN() );
                //ASSERT_THROWS( d.done().jsonString( Strict ), AssertionException );
                s = d.done().jsonString( Strict );
                ASSERT( str::contains(s, "NaN") );
            }
        };

        class NumberPrecision {
        public:
            void run() {
                BSONObjBuilder b;
                b.append( "a", 123456789 );
                ASSERT_EQUALS( "{ \"a\" : 123456789 }", b.done().jsonString( Strict ) );
            }
        };

        class NegativeNumber {
        public:
            void run() {
                BSONObjBuilder b;
                b.append( "a", -1 );
                ASSERT_EQUALS( "{ \"a\" : -1 }", b.done().jsonString( Strict ) );
            }
        };

        class SingleBoolMember {
        public:
            void run() {
                BSONObjBuilder b;
                b.appendBool( "a", true );
                ASSERT_EQUALS( "{ \"a\" : true }", b.done().jsonString( Strict ) );

                BSONObjBuilder c;
                c.appendBool( "a", false );
                ASSERT_EQUALS( "{ \"a\" : false }", c.done().jsonString( Strict ) );
            }
        };

        class SingleNullMember {
        public:
            void run() {
                BSONObjBuilder b;
                b.appendNull( "a" );
                ASSERT_EQUALS( "{ \"a\" : null }", b.done().jsonString( Strict ) );
            }
        };

        class SingleUndefinedMember {
        public:
            void run() {
                BSONObjBuilder b;
                b.appendUndefined( "a" );
                ASSERT_EQUALS( "{ \"a\" : { \"$undefined\" : true } }", b.done().jsonString( Strict ) );
                ASSERT_EQUALS( "{ \"a\" : undefined }", b.done().jsonString( JS ) );
                ASSERT_EQUALS( "{ \"a\" : undefined }", b.done().jsonString( TenGen ) );
            }
        };

        class SingleObjectMember {
        public:
            void run() {
                BSONObjBuilder b, c;
                b.append( "a", c.done() );
                ASSERT_EQUALS( "{ \"a\" : {} }", b.done().jsonString( Strict ) );
            }
        };

        class TwoMembers {
        public:
            void run() {
                BSONObjBuilder b;
                b.append( "a", 1 );
                b.append( "b", 2 );
                ASSERT_EQUALS( "{ \"a\" : 1, \"b\" : 2 }", b.done().jsonString( Strict ) );
            }
        };

        class EmptyArray {
        public:
            void run() {
                vector< int > arr;
                BSONObjBuilder b;
                b.append( "a", arr );
                ASSERT_EQUALS( "{ \"a\" : [] }", b.done().jsonString( Strict ) );
            }
        };

        class Array {
        public:
            void run() {
                vector< int > arr;
                arr.push_back( 1 );
                arr.push_back( 2 );
                BSONObjBuilder b;
                b.append( "a", arr );
                ASSERT_EQUALS( "{ \"a\" : [ 1, 2 ] }", b.done().jsonString( Strict ) );
            }
        };

        class DBRef {
        public:
            void run() {
                OID oid;
                memset( &oid, 0xff, 12 );
                BSONObjBuilder b;
                b.appendDBRef( "a", "namespace", oid );
                BSONObj built = b.done();
                ASSERT_EQUALS( "{ \"a\" : { \"$ref\" : \"namespace\", \"$id\" : \"ffffffffffffffffffffffff\" } }",
                               built.jsonString( Strict ) );
                ASSERT_EQUALS( "{ \"a\" : { \"$ref\" : \"namespace\", \"$id\" : \"ffffffffffffffffffffffff\" } }",
                               built.jsonString( JS ) );
                ASSERT_EQUALS( "{ \"a\" : Dbref( \"namespace\", \"ffffffffffffffffffffffff\" ) }",
                               built.jsonString( TenGen ) );
            }
        };

        class DBRefZero {
        public:
            void run() {
                OID oid;
                memset( &oid, 0, 12 );
                BSONObjBuilder b;
                b.appendDBRef( "a", "namespace", oid );
                ASSERT_EQUALS( "{ \"a\" : { \"$ref\" : \"namespace\", \"$id\" : \"000000000000000000000000\" } }",
                               b.done().jsonString( Strict ) );
            }
        };

        class ObjectId {
        public:
            void run() {
                OID oid;
                memset( &oid, 0xff, 12 );
                BSONObjBuilder b;
                b.appendOID( "a", &oid );
                BSONObj built = b.done();
                ASSERT_EQUALS( "{ \"a\" : { \"$oid\" : \"ffffffffffffffffffffffff\" } }",
                               built.jsonString( Strict ) );
                ASSERT_EQUALS( "{ \"a\" : ObjectId( \"ffffffffffffffffffffffff\" ) }",
                               built.jsonString( TenGen ) );
            }
        };

        class BinData {
        public:
            void run() {
                char z[ 3 ];
                z[ 0 ] = 'a';
                z[ 1 ] = 'b';
                z[ 2 ] = 'c';
                BSONObjBuilder b;
                b.appendBinData( "a", 3, BinDataGeneral, z );

                string o = b.done().jsonString( Strict );

                ASSERT_EQUALS( "{ \"a\" : { \"$binary\" : \"YWJj\", \"$type\" : \"00\" } }",
                               o );

                BSONObjBuilder c;
                c.appendBinData( "a", 2, BinDataGeneral, z );
                ASSERT_EQUALS( "{ \"a\" : { \"$binary\" : \"YWI=\", \"$type\" : \"00\" } }",
                               c.done().jsonString( Strict ) );

                BSONObjBuilder d;
                d.appendBinData( "a", 1, BinDataGeneral, z );
                ASSERT_EQUALS( "{ \"a\" : { \"$binary\" : \"YQ==\", \"$type\" : \"00\" } }",
                               d.done().jsonString( Strict ) );
            }
        };

        class Symbol {
        public:
            void run() {
                BSONObjBuilder b;
                b.appendSymbol( "a", "b" );
                ASSERT_EQUALS( "{ \"a\" : \"b\" }", b.done().jsonString( Strict ) );
            }
        };

        class Date {
        public:
            void run() {
                BSONObjBuilder b;
                b.appendDate( "a", 0 );
                BSONObj built = b.done();
                ASSERT_EQUALS( "{ \"a\" : { \"$date\" : 0 } }", built.jsonString( Strict ) );
                ASSERT_EQUALS( "{ \"a\" : Date( 0 ) }", built.jsonString( TenGen ) );
                ASSERT_EQUALS( "{ \"a\" : Date( 0 ) }", built.jsonString( JS ) );
            }
        };

        class DateNegative {
        public:
            void run() {
                BSONObjBuilder b;
                b.appendDate( "a", -1 );
                BSONObj built = b.done();
                ASSERT_EQUALS( "{ \"a\" : { \"$date\" : -1 } }", built.jsonString( Strict ) );
                ASSERT_EQUALS( "{ \"a\" : Date( -1 ) }", built.jsonString( TenGen ) );
                ASSERT_EQUALS( "{ \"a\" : Date( -1 ) }", built.jsonString( JS ) );
            }
        };

        class Regex {
        public:
            void run() {
                BSONObjBuilder b;
                b.appendRegex( "a", "abc", "i" );
                BSONObj built = b.done();
                ASSERT_EQUALS( "{ \"a\" : { \"$regex\" : \"abc\", \"$options\" : \"i\" } }",
                               built.jsonString( Strict ) );
                ASSERT_EQUALS( "{ \"a\" : /abc/i }", built.jsonString( TenGen ) );
                ASSERT_EQUALS( "{ \"a\" : /abc/i }", built.jsonString( JS ) );
            }
        };

        class RegexEscape {
        public:
            void run() {
                BSONObjBuilder b;
                b.appendRegex( "a", "/\"", "i" );
                BSONObj built = b.done();
                ASSERT_EQUALS( "{ \"a\" : { \"$regex\" : \"/\\\"\", \"$options\" : \"i\" } }",
                               built.jsonString( Strict ) );
                ASSERT_EQUALS( "{ \"a\" : /\\/\\\"/i }", built.jsonString( TenGen ) );
                ASSERT_EQUALS( "{ \"a\" : /\\/\\\"/i }", built.jsonString( JS ) );
            }
        };

        class RegexManyOptions {
        public:
            void run() {
                BSONObjBuilder b;
                b.appendRegex( "a", "z", "abcgimx" );
                BSONObj built = b.done();
                ASSERT_EQUALS( "{ \"a\" : { \"$regex\" : \"z\", \"$options\" : \"abcgimx\" } }",
                               built.jsonString( Strict ) );
                ASSERT_EQUALS( "{ \"a\" : /z/gim }", built.jsonString( TenGen ) );
                ASSERT_EQUALS( "{ \"a\" : /z/gim }", built.jsonString( JS ) );
            }
        };

        class CodeTests {
        public:
            void run() {
                BSONObjBuilder b;
                b.appendCode( "x" , "function(){ return 1; }" );
                BSONObj o = b.obj();
                ASSERT_EQUALS( "{ \"x\" : function(){ return 1; } }" , o.jsonString() );
            }
        };

        class TimestampTests {
        public:
            void run() {
                BSONObjBuilder b;
                b.appendTimestamp( "x" , 4000 , 10 );
                BSONObj o = b.obj();
                ASSERT_EQUALS( "{ \"x\" : { \"$timestamp\" : { \"t\" : 4, \"i\" : 10 } } }",
                        o.jsonString( Strict ) );
                ASSERT_EQUALS( "{ \"x\" : { \"$timestamp\" : { \"t\" : 4, \"i\" : 10 } } }",
                        o.jsonString( JS ) );
                ASSERT_EQUALS( "{ \"x\" : Timestamp( 4, 10 ) }", o.jsonString( TenGen ) );
            }
        };

        class NullString {
        public:
            void run() {
                BSONObjBuilder b;
                b.append( "x" , "a\0b" , 4 );
                BSONObj o = b.obj();
                ASSERT_EQUALS( "{ \"x\" : \"a\\u0000b\" }" , o.jsonString() );
            }
        };

        class AllTypes {
        public:
            void run() {
                OID oid;
                oid.init();

                BSONObjBuilder b;
                b.appendMinKey( "a" );
                b.append( "b" , 5.5 );
                b.append( "c" , "abc" );
                b.append( "e" , BSON( "x" << 1 ) );
                b.append( "f" , BSON_ARRAY( 1 << 2 << 3 ) );
                b.appendBinData( "g" , sizeof(AllTypes) , bdtCustom , (const void*)this );
                b.appendUndefined( "h" );
                b.append( "i" , oid );
                b.appendBool( "j" , 1 );
                b.appendDate( "k" , 123 );
                b.appendNull( "l" );
                b.appendRegex( "m" , "a" );
                b.appendDBRef( "n" , "foo" , oid );
                b.appendCode( "o" , "function(){}" );
                b.appendSymbol( "p" , "foo" );
                b.appendCodeWScope( "q" , "function(){}" , BSON("x" << 1 ) );
                b.append( "r" , (int)5 );
                b.appendTimestamp( "s" , 123123123123123LL );
                b.append( "t" , 12321312312LL );
                b.appendMaxKey( "u" );

                BSONObj o = b.obj();
                o.jsonString();
                //cout << o.jsonString() << endl;
            }
        };

    } // namespace JsonStringTests

    namespace FromJsonTests {

        class Base {
        public:
            virtual ~Base() {}
            void run() {
                ASSERT( fromjson( json() ).valid() );
                assertEquals( bson(), fromjson( json() ) );
                assertEquals( bson(), fromjson( bson().jsonString( Strict ) ) );
                assertEquals( bson(), fromjson( bson().jsonString( TenGen ) ) );
                assertEquals( bson(), fromjson( bson().jsonString( JS ) ) );
            }
        protected:
            virtual BSONObj bson() const = 0;
            virtual string json() const = 0;
        private:
            static void assertEquals( const BSONObj &expected, const BSONObj &actual ) {
                if ( expected.woCompare( actual ) ) {
                    out() << "want:" << expected.jsonString() << " size: " << expected.objsize() << endl;
                    out() << "got :" << actual.jsonString() << " size: " << actual.objsize() << endl;
                    out() << expected.hexDump() << endl;
                    out() << actual.hexDump() << endl;
                }
                ASSERT( !expected.woCompare( actual ) );
            }
        };

        class Bad {
        public:
            virtual ~Bad() {}
            void run() {
                ASSERT_THROWS( fromjson( json() ), MsgAssertionException );
            }
        protected:
            virtual string json() const = 0;
        };

        class Empty : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                return b.obj();
            }
            virtual string json() const {
                return "{}";
            }
        };

        class EmptyWithSpace : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                return b.obj();
            }
            virtual string json() const {
                return "{ }";
            }
        };

        class SingleString : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.append( "a", "b" );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : \"b\" }";
            }
        };

        class EmptyStrings : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.append( "", "" );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"\" : \"\" }";
            }
        };

        class ReservedFieldName : public Bad {
            virtual string json() const {
                return "{ \"$oid\" : \"b\" }";
            }
        };

        class ReservedFieldName1 : public Bad {
            virtual string json() const {
                return "{ \"$ref\" : \"b\" }";
            }
        };

        class NumberFieldName : public Bad {
            virtual string json() const {
                return "{ 0 : \"b\" }";
            }
        };

        class InvalidFieldName : public Bad {
            virtual string json() const {
                return "{ test.test : \"b\" }";
            }
        };

        class QuotedNullName : public Bad {
            virtual string json() const {
                return "{ \"nc\0nc\" : \"b\" }";
            }
        };

        class NoValue : public Bad {
            virtual string json() const {
                return "{ a : }";
            }
        };

        class InvalidValue : public Bad {
            virtual string json() const {
                return "{ a : a }";
            }
        };

        class OkDollarFieldName : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.append( "$where", 1 );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"$where\" : 1 }";
            }
        };

        class SingleNumber : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.append( "a", 1 );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : 1 }";
            }
        };

        class RealNumber : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.append( "a", strtod( "0.7", 0 ) );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : 0.7 }";
            }            
        };
        
        class FancyNumber : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.append( "a", strtod( "-4.4433e-2", 0 ) );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : -4.4433e-2 }";
            }
        };

        class TwoElements : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.append( "a", 1 );
                b.append( "b", "foo" );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : 1, \"b\" : \"foo\" }";
            }
        };

        class Subobject : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.append( "a", 1 );
                BSONObjBuilder c;
                c.append( "z", b.done() );
                return c.obj();
            }
            virtual string json() const {
                return "{ \"z\" : { \"a\" : 1 } }";
            }
        };

        class DeeplyNestedObject : public Base {
            virtual string buildJson(int depth) const {
                if (depth == 0) {
                    return "{\"0\":true}";
                }
                else {
                    std::stringstream ss;
                    ss << "{\"" << depth << "\":" << buildJson(depth - 1) << "}";
                    depth--;
                    return ss.str();
                }
            }
            virtual BSONObj buildBson(int depth) const {
                BSONObjBuilder builder;
                if (depth == 0) {
                    builder.append( "0", true );
                    return builder.obj();
                }
                else {
                    std::stringstream ss;
                    ss << depth;
                    depth--;
                    builder.append(ss.str(), buildBson(depth));
                    return builder.obj();
                }
            }
            virtual BSONObj bson() const {
                return buildBson(35);
            }
            virtual string json() const {
                return buildJson(35);
            }
        };

        class ArrayEmpty : public Base {
            virtual BSONObj bson() const {
                vector< int > arr;
                BSONObjBuilder b;
                b.append( "a", arr );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : [] }";
            }
        };

        class Array : public Base {
            virtual BSONObj bson() const {
                vector< int > arr;
                arr.push_back( 1 );
                arr.push_back( 2 );
                arr.push_back( 3 );
                BSONObjBuilder b;
                b.append( "a", arr );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : [ 1, 2, 3 ] }";
            }
        };

        class True : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.appendBool( "a", true );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : true }";
            }
        };

        class False : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.appendBool( "a", false );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : false }";
            }
        };

        class Null : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.appendNull( "a" );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : null }";
            }
        };

        class Undefined : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.appendUndefined( "a" );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : undefined }";
            }
        };

        class UndefinedStrict : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.appendUndefined( "a" );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : { \"$undefined\" : true } }";
            }
        };
        
        class UndefinedStrictBad : public Bad {
            virtual string json() const {
                return "{ \"a\" : { \"$undefined\" : false } }";
            }
        };

        class EscapedCharacters : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.append( "a", "\" \\ / \b \f \n \r \t \v" );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : \"\\\" \\\\ \\/ \\b \\f \\n \\r \\t \\v\" }";
            }
        };

        class NonEscapedCharacters : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.append( "a", "% { a z $ # '  " );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : \"\\% \\{ \\a \\z \\$ \\# \\' \\ \" }";
            }
        };

        class AllowedControlCharacter : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.append( "a", "\x7f" );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : \"\x7f\" }";
            }
        };

        class InvalidControlCharacter : public Bad {
            virtual string json() const {
                return "{ \"a\" : \"\x1f\" }";
            }
        };

        class NumbersInFieldName : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.append( "b1", "b" );
                return b.obj();
            }
            virtual string json() const {
                return "{ b1 : \"b\" }";
            }
        };

        class EscapeFieldName : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.append( "\n", "b" );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"\\n\" : \"b\" }";
            }
        };

        class EscapedUnicodeToUtf8 : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                unsigned char u[ 7 ];
                u[ 0 ] = 0xe0 | 0x0a;
                u[ 1 ] = 0x80;
                u[ 2 ] = 0x80;
                u[ 3 ] = 0xe0 | 0x0a;
                u[ 4 ] = 0x80;
                u[ 5 ] = 0x80;
                u[ 6 ] = 0;
                b.append( "a", (char *) u );
                BSONObj built = b.obj();
                ASSERT_EQUALS( string( (char *) u ), built.firstElement().valuestr() );
                return built;
            }
            virtual string json() const {
                return "{ \"a\" : \"\\ua000\\uA000\" }";
            }
        };

        class Utf8AllOnes : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                unsigned char u[ 8 ];
                u[ 0 ] = 0x01;

                u[ 1 ] = 0x7f;

                u[ 2 ] = 0xdf;
                u[ 3 ] = 0xbf;

                u[ 4 ] = 0xef;
                u[ 5 ] = 0xbf;
                u[ 6 ] = 0xbf;

                u[ 7 ] = 0;

                b.append( "a", (char *) u );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : \"\\u0001\\u007f\\u07ff\\uffff\" }";
            }
        };

        class Utf8FirstByteOnes : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                unsigned char u[ 6 ];
                u[ 0 ] = 0xdc;
                u[ 1 ] = 0x80;

                u[ 2 ] = 0xef;
                u[ 3 ] = 0xbc;
                u[ 4 ] = 0x80;

                u[ 5 ] = 0;

                b.append( "a", (char *) u );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : \"\\u0700\\uff00\" }";
            }
        };

        class Utf8Invalid : public Bad {
            virtual string json() const {
                return "{ \"a\" : \"\\u0ZZZ\" }";
            }
        };

        class Utf8TooShort : public Bad {
            virtual string json() const {
                return "{ \"a\" : \"\\u000\" }";
            }
        };

        class DBRefConstructor : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                BSONObjBuilder subBuilder(b.subobjStart("a"));
                subBuilder.append("$ref", "ns");
                subBuilder.append("$id", "000000000000000000000000");
                subBuilder.done();
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : Dbref( \"ns\", \"000000000000000000000000\" ) }";
            }
        };

        // Added for consistency with the mongo shell
        class DBRefConstructorCapitals : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                BSONObjBuilder subBuilder(b.subobjStart("a"));
                subBuilder.append("$ref", "ns");
                subBuilder.append("$id", "000000000000000000000000");
                subBuilder.done();
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : DBRef( \"ns\", \"000000000000000000000000\" ) }";
            }
        };

        class DBRefConstructorNumber : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                BSONObjBuilder subBuilder(b.subobjStart("a"));
                subBuilder.append("$ref", "ns");
                subBuilder.append("$id", 1);
                subBuilder.done();
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : Dbref( \"ns\", 1 ) }";
            }
        };

        class DBRefNumberId : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                BSONObjBuilder subBuilder(b.subobjStart("a"));
                subBuilder.append("$ref", "ns");
                subBuilder.append("$id", 1);
                subBuilder.done();
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : { \"$ref\" : \"ns\", \"$id\" : 1 } }";
            }
        };

        class DBRefStringId : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                BSONObjBuilder subBuilder(b.subobjStart("a"));
                subBuilder.append("$ref", "ns");
                subBuilder.append("$id", "000000000000000000000000");
                subBuilder.done();
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : { \"$ref\" : \"ns\", \"$id\" : \"000000000000000000000000\" } }";
            }
        };

        class DBRefObjectIDObject : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                OID o;
                memset( &o, 0, 12 );
                BSONObjBuilder subBuilder(b.subobjStart("a"));
                subBuilder.append("$ref", "ns");
                subBuilder.append("$id", o);
                subBuilder.done();
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : { \"$ref\" : \"ns\", \"$id\" : { \"$oid\" : \"000000000000000000000000\" } } }";
            }
        };

        class DBRefObjectIDConstructor : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                OID o;
                memset( &o, 0, 12 );
                BSONObjBuilder subBuilder(b.subobjStart("a"));
                subBuilder.append("$ref", "ns");
                subBuilder.append("$id", o);
                subBuilder.done();
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : { \"$ref\" : \"ns\", \"$id\" : ObjectId( \"000000000000000000000000\" ) } }";
            }
        };

        class Oid : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.appendOID( "_id" );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"_id\" : { \"$oid\" : \"000000000000000000000000\" } }";
            }
        };

        class Oid2 : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                OID o;
                memset( &o, 0x0f, 12 );
                b.appendOID( "_id", &o );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"_id\" : ObjectId( \"0f0f0f0f0f0f0f0f0f0f0f0f\" ) }";
            }
        };

        class OidTooLong : public Bad {
            virtual string json() const {
                return "{ \"_id\" : { \"$oid\" : \"0000000000000000000000000\" } }";
            }
        };

        class Oid2TooLong : public Bad {
            virtual string json() const {
                return "{ \"_id\" : ObjectId( \"0f0f0f0f0f0f0f0f0f0f0f0f0\" ) }";
            }
        };

        class OidTooShort : public Bad {
            virtual string json() const {
                return "{ \"_id\" : { \"$oid\" : \"00000000000000000000000\" } }";
            }
        };

        class Oid2TooShort : public Bad {
            virtual string json() const {
                return "{ \"_id\" : ObjectId( \"0f0f0f0f0f0f0f0f0f0f0f0\" ) }";
            }
        };

        class OidInvalidChar : public Bad {
            virtual string json() const {
                return "{ \"_id\" : { \"$oid\" : \"00000000000Z000000000000\" } }";
            }
        };

        class Oid2InvalidChar : public Bad {
            virtual string json() const {
                return "{ \"_id\" : ObjectId( \"0f0f0f0f0f0fZf0f0f0f0f0f\" ) }";
            }
        };

        class StringId : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.append("_id", "000000000000000000000000");
                return b.obj();
            }
            virtual string json() const {
                return "{ \"_id\" : \"000000000000000000000000\" }";
            }
        };

        class BinData : public Base {
            virtual BSONObj bson() const {
                char z[ 3 ];
                z[ 0 ] = 'a';
                z[ 1 ] = 'b';
                z[ 2 ] = 'c';
                BSONObjBuilder b;
                b.appendBinData( "a", 3, BinDataGeneral, z );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : { \"$binary\" : \"YWJj\", \"$type\" : \"00\" } }";
            }
        };

        class BinData1 : public Base {
            virtual BSONObj bson() const {
                char z[ 3 ];
                z[ 0 ] = 'a';
                z[ 1 ] = 'b';
                z[ 2 ] = 'c';
                BSONObjBuilder b;
                b.appendBinData( "a", 3, Function, z );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : { \"$binary\" : \"YWJj\", \"$type\" : \"01\" } }";
            }
        };

        class BinData2 : public Base {
            virtual BSONObj bson() const {
                char z[ 3 ];
                z[ 0 ] = 'a';
                z[ 1 ] = 'b';
                z[ 2 ] = 'c';
                BSONObjBuilder b;
                b.appendBinData( "a", 3, ByteArrayDeprecated, z );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : { \"$binary\" : \"YWJj\", \"$type\" : \"02\" } }";
            }
        };

        class BinData3 : public Base {
            virtual BSONObj bson() const {
                char z[ 3 ];
                z[ 0 ] = 'a';
                z[ 1 ] = 'b';
                z[ 2 ] = 'c';
                BSONObjBuilder b;
                b.appendBinData( "a", 3, bdtUUID, z );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : { \"$binary\" : \"YWJj\", \"$type\" : \"03\" } }";
            }
        };

        class BinData4 : public Base {
            virtual BSONObj bson() const {
                char z[ 3 ];
                z[ 0 ] = 'a';
                z[ 1 ] = 'b';
                z[ 2 ] = 'c';
                BSONObjBuilder b;
                b.appendBinData( "a", 3, newUUID, z );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : { \"$binary\" : \"YWJj\", \"$type\" : \"04\" } }";
            }
        };

        class BinData5 : public Base {
            virtual BSONObj bson() const {
                char z[ 3 ];
                z[ 0 ] = 'a';
                z[ 1 ] = 'b';
                z[ 2 ] = 'c';
                BSONObjBuilder b;
                b.appendBinData( "a", 3, MD5Type, z );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : { \"$binary\" : \"YWJj\", \"$type\" : \"05\" } }";
            }
        };

        class BinData80 : public Base {
            virtual BSONObj bson() const {
                char z[ 3 ];
                z[ 0 ] = 'a';
                z[ 1 ] = 'b';
                z[ 2 ] = 'c';
                BSONObjBuilder b;
                b.appendBinData( "a", 3, bdtCustom, z );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : { \"$binary\" : \"YWJj\", \"$type\" : \"80\" } }";
            }
        };

        class BinDataPaddedSingle : public Base {
            virtual BSONObj bson() const {
                char z[ 2 ];
                z[ 0 ] = 'a';
                z[ 1 ] = 'b';
                BSONObjBuilder b;
                b.appendBinData( "a", 2, BinDataGeneral, z );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : { \"$binary\" : \"YWI=\", \"$type\" : \"00\" } }";
            }
        };

        class BinDataPaddedDouble : public Base {
            virtual BSONObj bson() const {
                char z[ 1 ];
                z[ 0 ] = 'a';
                BSONObjBuilder b;
                b.appendBinData( "a", 1, BinDataGeneral, z );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : { \"$binary\" : \"YQ==\", \"$type\" : \"00\" } }";
            }
        };

        class BinDataAllChars : public Base {
            virtual BSONObj bson() const {
                unsigned char z[] = {
                    0x00, 0x10, 0x83, 0x10, 0x51, 0x87, 0x20, 0x92, 0x8B, 0x30,
                    0xD3, 0x8F, 0x41, 0x14, 0x93, 0x51, 0x55, 0x97, 0x61, 0x96,
                    0x9B, 0x71, 0xD7, 0x9F, 0x82, 0x18, 0xA3, 0x92, 0x59, 0xA7,
                    0xA2, 0x9A, 0xAB, 0xB2, 0xDB, 0xAF, 0xC3, 0x1C, 0xB3, 0xD3,
                    0x5D, 0xB7, 0xE3, 0x9E, 0xBB, 0xF3, 0xDF, 0xBF
                };
                BSONObjBuilder b;
                b.appendBinData( "a", 48, BinDataGeneral, z );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : { \"$binary\" : \"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/\", \"$type\" : \"00\" } }";
            }
        };

        class BinDataBadLength : public Bad {
            virtual string json() const {
                return "{ \"a\" : { \"$binary\" : \"YQ=\", \"$type\" : \"00\" } }";
            }
        };

        class BinDataBadLength1 : public Bad {
            virtual string json() const {
                return "{ \"a\" : { \"$binary\" : \"YQ\", \"$type\" : \"00\" } }";
            }
        };
        
        class BinDataBadLength2 : public Bad {
            virtual string json() const {
                return "{ \"a\" : { \"$binary\" : \"YQX==\", \"$type\" : \"00\" } }";
            }
        };

        class BinDataBadLength3 : public Bad {
            virtual string json() const {
                return "{ \"a\" : { \"$binary\" : \"YQX\", \"$type\" : \"00\" } }";
            }
        };

        class BinDataBadLength4 : public Bad {
            virtual string json() const {
                return "{ \"a\" : { \"$binary\" : \"YQXZ=\", \"$type\" : \"00\" } }";
            }
        };

        class BinDataBadLength5 : public Bad {
            virtual string json() const {
                return "{ \"a\" : { \"$binary\" : \"YQXZ==\", \"$type\" : \"00\" } }";
            }
        };

        class BinDataBadChars : public Bad {
            virtual string json() const {
                return "{ \"a\" : { \"$binary\" : \"a...\", \"$type\" : \"00\" } }";
            }
        };

        class BinDataTypeTooShort : public Bad {
            virtual string json() const {
                return "{ \"a\" : { \"$binary\" : \"AAAA\", \"$type\" : \"0\" } }";
            }
        };

        class BinDataTypeTooLong : public Bad {
            virtual string json() const {
                return "{ \"a\" : { \"$binary\" : \"AAAA\", \"$type\" : \"000\" } }";
            }
        };

        class BinDataTypeBadChars : public Bad {
            virtual string json() const {
                return "{ \"a\" : { \"$binary\" : \"AAAA\", \"$type\" : \"ZZ\" } }";
            }
        };

        class BinDataEmptyType : public Bad {
            virtual string json() const {
                return "{ \"a\" : { \"$binary\" : \"AAAA\", \"$type\" : \"\" } }";
            }
        };

        class BinDataNoType : public Bad {
            virtual string json() const {
                return "{ \"a\" : { \"$binary\" : \"AAAA\" } }";
            }
        };

        class BinDataInvalidType : public Bad {
            virtual string json() const {
                return "{ \"a\" : { \"$binary\" : \"AAAA\", \"$type\" : \"100\" } }";
            }
        };

        class Date : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.appendDate( "a", 0 );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : { \"$date\" : 0 } }";
            }
        };

        class DateNegZero : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.appendDate( "a", -0 );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : { \"$date\" : -0 } }";
            }
        };

        class DateNonzero : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.appendDate( "a", 100 );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : { \"$date\" : 100 } }";
            }
        };

        class DateStrictTooLong : public Bad {
            virtual string json() const {
                stringstream ss;
                ss << "{ \"a\" : { \"$date\" : " << ~(0ULL) << "1" << " } }";
                return ss.str();
            }
        };

        class DateTooLong : public Bad {
            virtual string json() const {
                stringstream ss;
                ss << "{ \"a\" : Date( " << ~(0ULL) << "1" << " ) }";
                return ss.str();
            }
        };

        class DateIsString : public Bad {
            virtual string json() const {
                stringstream ss;
                ss << "{ \"a\" : { \"$date\" : \"100\" } }";
                return ss.str();
            }
        };

        class DateIsString1 : public Bad {
            virtual string json() const {
                stringstream ss;
                ss << "{ \"a\" : Date(\"a\") }";
                return ss.str();
            }
        };

        class DateIsString2 : public Bad {
            virtual string json() const {
                stringstream ss;
                ss << "{ \"a\" : new Date(\"a\") }";
                return ss.str();
            }
        };

        class DateIsFloat : public Bad {
            virtual string json() const {
                stringstream ss;
                ss << "{ \"a\" : { \"$date\" : 1.1 } }";
                return ss.str();
            }
        };
        
        class DateIsFloat1 : public Bad {
            virtual string json() const {
                stringstream ss;
                ss << "{ \"a\" : Date(1.1) }";
                return ss.str();
            }
        };

        class DateIsFloat2 : public Bad {
            virtual string json() const {
                stringstream ss;
                ss << "{ \"a\" : new Date(1.1) }";
                return ss.str();
            }
        };

        class DateIsExponent : public Bad {
            virtual string json() const {
                stringstream ss;
                ss << "{ \"a\" : { \"$date\" : 10e3 } }";
                return ss.str();
            }
        };

        class DateIsExponent1 : public Bad {
            virtual string json() const {
                stringstream ss;
                ss << "{ \"a\" : Date(10e3) }";
                return ss.str();
            }
        };

        class DateIsExponent2 : public Bad {
            virtual string json() const {
                stringstream ss;
                ss << "{ \"a\" : new Date(10e3) }";
                return ss.str();
            }
        };
        /* Need to handle this because jsonString outputs the value of Date_t as unsigned.
         * See SERVER-8330 and SERVER-8573 */
        class DateStrictMaxUnsigned : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.appendDate( "a", -1 );
                return b.obj();
            }
            virtual string json() const {
                stringstream ss;
                ss << "{ \"a\" : { \"$date\" : "
                   << std::numeric_limits<unsigned long long>::max() << " } }";
                return ss.str();
            }
        };

        class DateMaxUnsigned : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.appendDate( "a", -1 );
                return b.obj();
            }
            virtual string json() const {
                stringstream ss;
                ss << "{ \"a\" : Date( "
                   << std::numeric_limits<unsigned long long>::max() << " ) }";
                return ss.str();
            }
        };

        class DateStrictNegative : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.appendDate( "a", -1 );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : { \"$date\" : -1 } }";
            }
        };

        class DateNegative : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.appendDate( "a", -1 );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : Date( -1 ) }";
            }
        };

        class Timestamp : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.appendTimestamp( "a", (unsigned long long) 20000, 5 );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : Timestamp( 20, 5 ) }";
            }
        };

        class TimestampNoIncrement : public Bad {
            virtual string json() const {
                return "{ \"a\" : Timestamp( 20 ) }";
            }
        };

        class TimestampZero : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.appendTimestamp( "a", 0ULL, 0 );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : Timestamp( 0, 0 ) }";
            }
        };

        class TimestampNoArgs : public Bad {
            virtual string json() const {
                return "{ \"a\" : Timestamp() }";
            }
        };

        class TimestampFloatSeconds : public Bad {
            virtual string json() const {
                return "{ \"a\" : Timestamp( 20.0, 1 ) }";
            }
        };

        class TimestampFloatIncrement : public Bad {
            virtual string json() const {
                return "{ \"a\" : Timestamp( 20, 1.0 ) }";
            }
        };

        class TimestampNegativeSeconds : public Bad {
            virtual string json() const {
                return "{ \"a\" : Timestamp( -20, 5 ) }";
            }
        };

        class TimestampNegativeIncrement : public Bad {
            virtual string json() const {
                return "{ \"a\" : Timestamp( 20, -5 ) }";
            }
        };

        class TimestampInvalidSeconds : public Bad {
            virtual string json() const {
                return "{ \"a\" : Timestamp( q, 5 ) }";
            }
        };

        class TimestampObject : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.appendTimestamp( "a", (unsigned long long) 20000, 5 );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : { \"$timestamp\" : { \"t\" : 20 , \"i\" : 5 } } }";
            }
        };

        class TimestampObjectInvalidFieldName : public Bad {
            virtual string json() const {
                return "{ \"a\" : { \"$timestamp\" : { \"time\" : 20 , \"increment\" : 5 } } }";
            }
        };

        class TimestampObjectNoIncrement : public Bad {
            virtual string json() const {
                return "{ \"a\" : { \"$timestamp\" : { \"t\" : 20 } } }";
            }
        };

        class TimestampObjectNegativeSeconds : public Bad {
            virtual string json() const {
                return "{ \"a\" : { \"$timestamp\" : { \"t\" : -20 , \"i\" : 5 } } }";
            }
        };

        class TimestampObjectNegativeIncrement : public Bad {
            virtual string json() const {
                return "{ \"a\" : { \"$timestamp\" : { \"t\" : 20 , \"i\" : -5 } } }";
            }
        };

        class TimestampObjectInvalidSeconds : public Bad {
            virtual string json() const {
                return "{ \"a\" : { \"$timestamp\" : { \"t\" : q , \"i\" : 5 } } }";
            }
        };

        class TimestampObjectZero : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.appendTimestamp( "a", 0ULL, 0 );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : { \"$timestamp\" : { \"t\" : 0, \"i\" : 0} } }";
            }
        };

        class TimestampObjectNoArgs : public Bad {
            virtual string json() const {
                return "{ \"a\" : { \"$timestamp\" : { } } }";
            }
        };

        class TimestampObjectFloatSeconds : public Bad {
            virtual string json() const {
                return "{ \"a\" : { \"$timestamp\" : { \"t\" : 1.0, \"i\" : 0} } }";
            }
        };

        class TimestampObjectFloatIncrement : public Bad {
            virtual string json() const {
                return "{ \"a\" : { \"$timestamp\" : { \"t\" : 20, \"i\" : 1.0} } }";
            }
        };

        class Regex : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.appendRegex( "a", "b", "i" );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : { \"$regex\" : \"b\", \"$options\" : \"i\" } }";
            }
        };

        class RegexNoOptionField : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.appendRegex( "a", "b", "" );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : { \"$regex\" : \"b\" } }";
            }
        };

        class RegexEscape : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.appendRegex( "a", "\t", "i" );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : { \"$regex\" : \"\\t\", \"$options\" : \"i\" } }";
            }
        };

        class RegexWithQuotes : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.appendRegex( "a", "\"", "" );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : /\"/ }";
            }
        };
        
        class RegexWithQuotes1 : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.appendRegex( "a", "\"", "" );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : { $regex : \"\\\"\" }}";
            }
        };

        class RegexInvalidField : public Bad {
            virtual string json() const {
                return "{ \"a\" : { \"$regex\" : \"b\", \"field\" : \"i\" } }";
            }
        };

        class RegexInvalidOption : public Bad {
            virtual string json() const {
                return "{ \"a\" : { \"$regex\" : \"b\", \"$options\" : \"1\" } }";
            }
        };

        class RegexInvalidOption2 : public Bad {
            virtual string json() const {
                return "{ \"a\" : /b/c }";
            }
        };

        class RegexInvalidOption3 : public Bad {
            virtual string json() const {
                return "{ \"a\" : /b/ic }";
            }
        };

        class RegexInvalidOption4 : public Bad {
            virtual string json() const {
                return "{ \"a\" : { \"$regex\" : \"b\", \"$options\" : \"a\" } }";
            }
        };

        class RegexInvalidOption5 : public Bad {
            virtual string json() const {
                return "{ \"a\" : /b/a }";
            }
        };

        class RegexEmptyOption : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.appendRegex( "a", "b", "" );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : { \"$regex\" : \"b\", \"$options\" : \"\" } }";
            }
        };

        class RegexEmpty : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.appendRegex("a", "", "");
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" : { \"$regex\" : \"\", \"$options\" : \"\"} }";
            }
        };

        class RegexEmpty1 : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.appendRegex("a", "", "");
                return b.obj();
            }
            virtual string json() const {
                return "{ \"a\" :  //  }";
            }
        };

        class RegexOverlap : public Bad {
            virtual string json() const {
                return "{ \"a\" : { \"$regex\" : // } }";
            }
        };

        class Malformed : public Bad {
            string json() const {
                return "{";
            }
        };

        class Malformed1 : public Bad {
            string json() const {
                return "}";
            }
        };

        class Malformed2 : public Bad {
            string json() const {
                return "{test}";
            }
        };

        class Malformed3 : public Bad {
            string json() const {
                return "{test";
            }
        };

        class Malformed4 : public Bad {
            string json() const {
                return "{ test : 1";
            }
        };
        
        class Malformed5 : public Bad {
            string json() const {
                return "{ test : 1 , }";
            }
        };

        class Malformed6 : public Bad {
            string json() const {
                return "{ test : 1 , tst}";
            }
        };
        
        class Malformed7 : public Bad {
            string json() const {
                return "{ a : []";
            }
        };

        class Malformed8 : public Bad {
            string json() const {
                return "{ a : { test : 1 }";
            }
        };
        
        class Malformed9 : public Bad {
            string json() const {
                return "{ a : [ { test : 1]}";
            }
        };

        class Malformed10 : public Bad {
            string json() const {
                return "{ a : [ { test : 1], b : 2}";
            }
        };

        class Malformed11 : public Bad {
            string json() const {
                return "{ a : \"test\"string }";
            }
        };

        class Malformed12 : public Bad {
            string json() const {
                return "{ a : test\"string\" }";
            }
        };

        class Malformed13 : public Bad {
            string json() const {
                return "{ a\"bad\" : \"teststring\" }";
            }
        };

        class Malformed14 : public Bad {
            string json() const {
                return "{ \"a\"test : \"teststring\" }";
            }
        };

        class Malformed15 : public Bad {
            string json() const {
                return "{ \"atest : \"teststring\" }";
            }
        };

        class Malformed16 : public Bad {
            string json() const {
                return "{ atest\" : \"teststring\" }";
            }
        };

        class Malformed17 : public Bad {
            string json() const {
                return "{ atest\" : 1 }";
            }
        };

        class Malformed18 : public Bad {
            string json() const {
                return "{ atest : \"teststring }";
            }
        };

        class Malformed19 : public Bad {
            string json() const {
                return "{ atest : teststring\" }";
            }
        };

        class UnquotedFieldName : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.append( "a_b", 1 );
                return b.obj();
            }
            virtual string json() const {
                return "{ a_b : 1 }";
            }
        };

        class UnquotedFieldNameBad : public Bad {
            string json() const {
                return "{ 123 : 1 }";
            }
        };
         
        class UnquotedFieldNameBad1 : public Bad {
            string json() const {
                return "{ -123 : 1 }";
            }
        };

        class UnquotedFieldNameBad2 : public Bad {
            string json() const {
                return "{ .123 : 1 }";
            }
        };

        class UnquotedFieldNameBad3 : public Bad {
            string json() const {
                return "{ -.123 : 1 }";
            }
        };

        class UnquotedFieldNameBad4 : public Bad {
            string json() const {
                return "{ -1.23 : 1 }";
            }
        };

        class UnquotedFieldNameBad5 : public Bad {
            string json() const {
                return "{ 1e23 : 1 }";
            }
        };
        
        class UnquotedFieldNameBad6 : public Bad {
            string json() const {
                return "{ -1e23 : 1 }";
            }
        };

        class UnquotedFieldNameBad7 : public Bad {
            string json() const {
                return "{ -1e-23 : 1 }";
            }
        };

        class UnquotedFieldNameBad8 : public Bad {
            string json() const {
                return "{ -hello : 1 }";
            }
        };

        class UnquotedFieldNameBad9 : public Bad {
            string json() const {
                return "{ il.legal : 1 }";
            }
        };

        class UnquotedFieldNameBad10 : public Bad {
            string json() const {
                return "{ 10gen : 1 }";
            }
        };

        class UnquotedFieldNameBad11 : public Bad {
            string json() const {
                return "{ _123. : 1 }";
            }
        };

        class UnquotedFieldNameBad12 : public Bad {
            string json() const {
                return "{ he-llo : 1 }";
            }
        };

        class UnquotedFieldNameBad13 : public Bad {
            string json() const {
                return "{ bad\nchar : 1 }";
            }
        };

        class UnquotedFieldNameBad14 : public Bad {
            string json() const {
                return "{ thiswill\fail : 1 }";
            }
        };

        class UnquotedFieldNameBad15 : public Bad {
            string json() const {
                return "{ failu\re : 1 }";
            }
        };

        class UnquotedFieldNameBad16 : public Bad {
            string json() const {
                return "{ t\test : 1 }";
            }
        };

        class UnquotedFieldNameBad17 : public Bad {
            string json() const {
                return "{ \break: 1 }";
            }
        };

        class UnquotedFieldNameBad18 : public Bad {
            string json() const {
                //here we fill the memory directly to test unicode values 
                //In this case we set \u0700 and \uFF00
                //Setting it directly in memory avoids MSVC error c4566
                unsigned char u[ 6 ];
                u[ 0 ] = 0xdc;
                u[ 1 ] = 0x80;

                u[ 2 ] = 0xef;
                u[ 3 ] = 0xbc;
                u[ 4 ] = 0x80;

                u[ 5 ] = 0;
                std::stringstream ss;
                ss << "{ " << u << " : 1 }";
                return ss.str();
            }
        };

        class UnquotedFieldNameBad19 : public Bad {
            string json() const {
                return "{ bl\\u3333p: 1 }";
            }
        };

        class UnquotedFieldNameBad20 : public Bad {
            string json() const {
                return "{ bl-33p: 1 }";
            }
        };

        class UnquotedFieldNameDollar : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.append( "$a_b", 1 );
                return b.obj();
            }
            virtual string json() const {
                return "{ $a_b : 1 }";
            }
        };

        class SingleQuotes : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.append( "ab'c\"", "bb\b '\"" );
                return b.obj();
            }
            virtual string json() const {
                return "{ 'ab\\'c\"' : 'bb\\b \\'\"' }";
            }
        };

        class QuoteTest : public Base { 
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.append("\"", "test");
                return b.obj();
            }
            virtual string json() const { 
                return "{ '\"' : \"test\" }";
            }       
        };

        class QuoteTest1 : public Base { 
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.append("'", "test");
                return b.obj();
            }
            virtual string json() const { 
                return "{ \"'\" : \"test\" }";
            }       
        };

        class QuoteTest2 : public Base { 
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.append("\"", "test");
                return b.obj();
            }
            virtual string json() const { 
                return "{ '\"' : \"test\" }";
            }       
        };

        class QuoteTest3 : public Base { 
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.append("\"'\"", "test");
                return b.obj();
            }
            virtual string json() const { 
                return "{ '\"\\\'\"' : \"test\" }";
            }       
        };

        class QuoteTest4 : public Base { 
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.append("'\"'", "test");
                return b.obj();
            }
            virtual string json() const { 
                return "{ \"'\\\"'\" : \"test\" }";
            }       
        };

        class QuoteTest5 : public Base { 
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.append("test", "'");
                return b.obj();
            }
            virtual string json() const { 
                return "{ \"test\" : \"'\" }";
            }       
        };

        class QuoteTest6 : public Base { 
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.append("test", "\"");
                return b.obj();
            }
            virtual string json() const { 
                return "{ \"test\" : '\"' }";
            }       
        };

        class ObjectId : public Base {
            virtual BSONObj bson() const {
                OID id;
                id.init( "deadbeeff00ddeadbeeff00d" );
                BSONObjBuilder b;
                b.appendOID( "_id", &id );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"_id\": ObjectId( \"deadbeeff00ddeadbeeff00d\" ) }";
            }
        };

        class ObjectId2 : public Base {
            virtual BSONObj bson() const {
                OID id;
                id.init( "deadbeeff00ddeadbeeff00d" );
                BSONObjBuilder b;
                b.appendOID( "foo", &id );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"foo\": ObjectId( \"deadbeeff00ddeadbeeff00d\" ) }";
            }
        };

        class NumericTypes : public Base {
        public:
            void run() {
                Base::run();

                BSONObj o = fromjson(json());

                ASSERT(o["int"].type() == NumberInt);
                ASSERT(o["long"].type() == NumberLong);
                ASSERT(o["double"].type() == NumberDouble);

                ASSERT(o["long"].numberLong() == 9223372036854775807ll);
            }

            virtual BSONObj bson() const {
                return BSON( "int" << 123
                             << "long" << 9223372036854775807ll // 2**63 - 1
                             << "double" << 3.14
                           );
            }
            virtual string json() const {
                return "{ \"int\": 123, \"long\": 9223372036854775807, \"double\": 3.14 }";
            }
        };

        class NumericLimits : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder builder;
                BSONArrayBuilder numArray(builder.subarrayStart(""));
                numArray.append(std::numeric_limits<long long>::max());
                numArray.append(std::numeric_limits<long long>::min());
                numArray.append(std::numeric_limits<int>::max());
                numArray.append(std::numeric_limits<int>::min());
                numArray.done();
                return builder.obj();
            }
            virtual string json() const {
                std::stringstream ss;
                ss << "{'': [";
                ss << std::numeric_limits<long long>::max() << ",";
                ss << std::numeric_limits<long long>::min() << ",";
                ss << std::numeric_limits<int>::max() << ",";
                ss << std::numeric_limits<int>::min();
                ss << "] }";
                return ss.str();
            }
        };

        //Overflows double by giving it an exponent that is too large
        class NumericLimitsBad : public Bad {
            virtual string json() const {
                std::stringstream ss;
                ss << "{ test : ";
                ss << std::numeric_limits<double>::max()  << "1111111111";
                ss << "}";
                return ss.str();
            }
        };

        class NumericLimitsBad1 : public Bad {
            virtual string json() const {
                std::stringstream ss;
                ss << "{ test : ";
                ss << std::numeric_limits<double>::min() << "11111111111";
                ss << "}";
                return ss.str();
            }
        };

        class NegativeNumericTypes : public Base {
        public:
            void run() {
                Base::run();

                BSONObj o = fromjson(json());

                ASSERT(o["int"].type() == NumberInt);
                ASSERT(o["long"].type() == NumberLong);
                ASSERT(o["double"].type() == NumberDouble);

                ASSERT(o["long"].numberLong() == -9223372036854775807ll);
            }

            virtual BSONObj bson() const {
                return BSON( "int" << -123
                             << "long" << -9223372036854775807ll // -1 * (2**63 - 1)
                             << "double" << -3.14
                           );
            }
            virtual string json() const {
                return "{ \"int\": -123, \"long\": -9223372036854775807, \"double\": -3.14 }";
            }
        };

        class EmbeddedDatesBase : public Base  {
        public:

            virtual void run() {
                BSONObj o = fromjson( json() );
                ASSERT_EQUALS( 3 , (o["time.valid"].type()) );
                BSONObj e = o["time.valid"].embeddedObjectUserCheck();
                ASSERT_EQUALS( 9 , e["$gt"].type() );
                ASSERT_EQUALS( 9 , e["$lt"].type() );
                Base::run();
            }

            BSONObj bson() const {
                BSONObjBuilder e;
                e.appendDate( "$gt" , 1257829200000LL );
                e.appendDate( "$lt" , 1257829200100LL );

                BSONObjBuilder b;
                b.append( "time.valid" , e.obj() );
                return b.obj();
            }
            virtual string json() const = 0;
        };

        struct EmbeddedDatesFormat1 :  EmbeddedDatesBase  {
            string json() const {
                return "{ \"time.valid\" : { $gt : { \"$date\" :  1257829200000 } , $lt : { \"$date\" : 1257829200100 } } }";
            }
        };
        struct EmbeddedDatesFormat2 :  EmbeddedDatesBase  {
            string json() const {
                return "{ \"time.valid\" : { $gt : Date(1257829200000) , $lt : Date( 1257829200100 ) } }";
            }
        };
        struct EmbeddedDatesFormat3 :  EmbeddedDatesBase  {
            string json() const {
                return "{ \"time.valid\" : { $gt : new Date(1257829200000) , $lt : new Date( 1257829200100 ) } }";
            }
        };

        class NullString : public Base {
            virtual BSONObj bson() const {
                BSONObjBuilder b;
                b.append( "x" , "a\0b" , 4 );
                return b.obj();
            }
            virtual string json() const {
                return "{ \"x\" : \"a\\u0000b\" }";
            }
        };

        class NullFieldUnquoted : public Bad {
            virtual string json() const {
                return "{ x\\u0000y : \"a\" }";
            }
        };

    } // namespace FromJsonTests

    class All : public Suite {
    public:
        All() : Suite( "json" ) {
        }

        void setupTests() {
            add< JsonStringTests::Empty >();
            add< JsonStringTests::SingleStringMember >();
            add< JsonStringTests::EscapedCharacters >();
            add< JsonStringTests::AdditionalControlCharacters >();
            add< JsonStringTests::ExtendedAscii >();
            add< JsonStringTests::EscapeFieldName >();
            add< JsonStringTests::SingleIntMember >();
            add< JsonStringTests::SingleNumberMember >();
            add< JsonStringTests::InvalidNumbers >();
            add< JsonStringTests::NumberPrecision >();
            add< JsonStringTests::NegativeNumber >();
            add< JsonStringTests::SingleBoolMember >();
            add< JsonStringTests::SingleNullMember >();
            add< JsonStringTests::SingleUndefinedMember >();
            add< JsonStringTests::SingleObjectMember >();
            add< JsonStringTests::TwoMembers >();
            add< JsonStringTests::EmptyArray >();
            add< JsonStringTests::Array >();
            add< JsonStringTests::DBRef >();
            add< JsonStringTests::DBRefZero >();
            add< JsonStringTests::ObjectId >();
            add< JsonStringTests::BinData >();
            add< JsonStringTests::Symbol >();
            add< JsonStringTests::Date >();
            add< JsonStringTests::DateNegative >();
            add< JsonStringTests::Regex >();
            add< JsonStringTests::RegexEscape >();
            add< JsonStringTests::RegexManyOptions >();
            add< JsonStringTests::CodeTests >();
            add< JsonStringTests::TimestampTests >();
            add< JsonStringTests::NullString >();
            add< JsonStringTests::AllTypes >();

            add< FromJsonTests::Empty >();
            add< FromJsonTests::EmptyWithSpace >();
            add< FromJsonTests::SingleString >();
            add< FromJsonTests::EmptyStrings >();
            add< FromJsonTests::ReservedFieldName >();
            add< FromJsonTests::ReservedFieldName1 >();
            add< FromJsonTests::NumberFieldName >();
            add< FromJsonTests::InvalidFieldName >();
            add< FromJsonTests::QuotedNullName >();
            add< FromJsonTests::NoValue >();
            add< FromJsonTests::InvalidValue >();
            add< FromJsonTests::InvalidValue >();
            add< FromJsonTests::OkDollarFieldName >();
            add< FromJsonTests::SingleNumber >();
            add< FromJsonTests::RealNumber >();
            add< FromJsonTests::FancyNumber >();
            add< FromJsonTests::TwoElements >();
            add< FromJsonTests::Subobject >();
            add< FromJsonTests::DeeplyNestedObject >();
            add< FromJsonTests::ArrayEmpty >();
            add< FromJsonTests::Array >();
            add< FromJsonTests::True >();
            add< FromJsonTests::False >();
            add< FromJsonTests::Null >();
            add< FromJsonTests::Undefined >();
            add< FromJsonTests::UndefinedStrict >();
            add< FromJsonTests::UndefinedStrictBad >();
            add< FromJsonTests::EscapedCharacters >();
            add< FromJsonTests::NonEscapedCharacters >();
            add< FromJsonTests::AllowedControlCharacter >();
            add< FromJsonTests::InvalidControlCharacter >();
            add< FromJsonTests::NumbersInFieldName >();
            add< FromJsonTests::EscapeFieldName >();
            add< FromJsonTests::EscapedUnicodeToUtf8 >();
            add< FromJsonTests::Utf8AllOnes >();
            add< FromJsonTests::Utf8FirstByteOnes >();
            add< FromJsonTests::Utf8Invalid >();
            add< FromJsonTests::Utf8TooShort >();
            add< FromJsonTests::DBRefConstructor >();
            add< FromJsonTests::DBRefConstructorCapitals >();
            add< FromJsonTests::DBRefNumberId >();
            add< FromJsonTests::DBRefStringId >();
            add< FromJsonTests::DBRefObjectIDObject >();
            add< FromJsonTests::DBRefObjectIDConstructor >();
            add< FromJsonTests::Oid >();
            add< FromJsonTests::Oid2 >();
            add< FromJsonTests::OidTooLong >();
            add< FromJsonTests::Oid2TooLong >();
            add< FromJsonTests::OidTooShort >();
            add< FromJsonTests::Oid2TooShort >();
            add< FromJsonTests::OidInvalidChar >();
            add< FromJsonTests::Oid2InvalidChar >();
            add< FromJsonTests::StringId >();
            add< FromJsonTests::BinData >();
            add< FromJsonTests::BinData1 >();
            add< FromJsonTests::BinData2 >();
            add< FromJsonTests::BinData3 >();
            add< FromJsonTests::BinData4 >();
            add< FromJsonTests::BinData5 >();
            add< FromJsonTests::BinData80 >();
            add< FromJsonTests::BinDataPaddedSingle >();
            add< FromJsonTests::BinDataPaddedDouble >();
            add< FromJsonTests::BinDataAllChars >();
            add< FromJsonTests::BinDataBadLength >();
            add< FromJsonTests::BinDataBadLength1 >();
            add< FromJsonTests::BinDataBadLength2 >();
            add< FromJsonTests::BinDataBadLength3 >();
            add< FromJsonTests::BinDataBadLength4 >();
            add< FromJsonTests::BinDataBadLength5 >();
            add< FromJsonTests::BinDataBadChars >();
            add< FromJsonTests::BinDataTypeTooShort >();
            add< FromJsonTests::BinDataTypeTooLong >();
            add< FromJsonTests::BinDataTypeBadChars >();
            add< FromJsonTests::BinDataEmptyType >();
            add< FromJsonTests::BinDataNoType >();
            add< FromJsonTests::BinDataInvalidType >();
            add< FromJsonTests::Date >();
            add< FromJsonTests::DateNegZero >();
            add< FromJsonTests::DateNonzero >();
            add< FromJsonTests::DateStrictTooLong >();
            add< FromJsonTests::DateTooLong >();
            add< FromJsonTests::DateIsString >();
            add< FromJsonTests::DateIsString1 >();
            add< FromJsonTests::DateIsString2 >();
            add< FromJsonTests::DateIsFloat >();
            add< FromJsonTests::DateIsFloat1 >();
            add< FromJsonTests::DateIsFloat2 >();
            add< FromJsonTests::DateIsExponent >();
            add< FromJsonTests::DateIsExponent1 >();
            add< FromJsonTests::DateIsExponent2 >();
            add< FromJsonTests::DateStrictMaxUnsigned >();
            add< FromJsonTests::DateMaxUnsigned >();
            add< FromJsonTests::DateStrictNegative >();
            add< FromJsonTests::DateNegative >();
            add< FromJsonTests::Timestamp >();
            add< FromJsonTests::TimestampNoIncrement >();
            add< FromJsonTests::TimestampZero >();
            add< FromJsonTests::TimestampNoArgs >();
            add< FromJsonTests::TimestampFloatSeconds >();
            add< FromJsonTests::TimestampFloatIncrement >();
            add< FromJsonTests::TimestampNegativeSeconds >();
            add< FromJsonTests::TimestampNegativeIncrement >();
            add< FromJsonTests::TimestampInvalidSeconds >();
            add< FromJsonTests::TimestampObject >();
            add< FromJsonTests::TimestampObjectInvalidFieldName >();
            add< FromJsonTests::TimestampObjectNoIncrement >();
            add< FromJsonTests::TimestampObjectNegativeSeconds >();
            add< FromJsonTests::TimestampObjectNegativeIncrement >();
            add< FromJsonTests::TimestampObjectInvalidSeconds >();
            add< FromJsonTests::TimestampObjectZero >();
            add< FromJsonTests::TimestampObjectNoArgs >();
            add< FromJsonTests::TimestampObjectFloatSeconds >();
            add< FromJsonTests::TimestampObjectFloatIncrement >();
            add< FromJsonTests::Regex >();
            add< FromJsonTests::RegexNoOptionField >();
            add< FromJsonTests::RegexEscape >();
            add< FromJsonTests::RegexWithQuotes >();
            add< FromJsonTests::RegexWithQuotes1 >();
            add< FromJsonTests::RegexInvalidField >();
            add< FromJsonTests::RegexInvalidOption >();
            add< FromJsonTests::RegexInvalidOption2 >();
            add< FromJsonTests::RegexInvalidOption3 >();
            add< FromJsonTests::RegexInvalidOption4 >();
            add< FromJsonTests::RegexInvalidOption5 >();
            add< FromJsonTests::RegexEmptyOption >();
            add< FromJsonTests::RegexEmpty >();
            add< FromJsonTests::RegexEmpty1 >();
            add< FromJsonTests::RegexOverlap >();
            add< FromJsonTests::Malformed >();
            add< FromJsonTests::Malformed1 >();
            add< FromJsonTests::Malformed2 >();
            add< FromJsonTests::Malformed3 >();
            add< FromJsonTests::Malformed4 >();
            add< FromJsonTests::Malformed5 >();
            add< FromJsonTests::Malformed6 >();
            add< FromJsonTests::Malformed7 >();
            add< FromJsonTests::Malformed8 >();
            add< FromJsonTests::Malformed9 >();
            add< FromJsonTests::Malformed10 >();
            add< FromJsonTests::Malformed11 >();
            add< FromJsonTests::Malformed12 >();
            add< FromJsonTests::Malformed13 >();
            add< FromJsonTests::Malformed14 >();
            add< FromJsonTests::Malformed15 >();
            add< FromJsonTests::Malformed16 >();
            add< FromJsonTests::Malformed17 >();
            add< FromJsonTests::Malformed18 >();
            add< FromJsonTests::Malformed19 >();
            add< FromJsonTests::UnquotedFieldName >();
            add< FromJsonTests::UnquotedFieldNameBad >();
            add< FromJsonTests::UnquotedFieldNameBad1 >();
            add< FromJsonTests::UnquotedFieldNameBad2 >();
            add< FromJsonTests::UnquotedFieldNameBad3 >();
            add< FromJsonTests::UnquotedFieldNameBad4 >();
            add< FromJsonTests::UnquotedFieldNameBad5 >();
            add< FromJsonTests::UnquotedFieldNameBad6 >();
            add< FromJsonTests::UnquotedFieldNameBad7 >();
            add< FromJsonTests::UnquotedFieldNameBad8 >();
            add< FromJsonTests::UnquotedFieldNameBad9 >();
            add< FromJsonTests::UnquotedFieldNameBad10 >();
            add< FromJsonTests::UnquotedFieldNameBad11 >();
            add< FromJsonTests::UnquotedFieldNameBad12 >();
            add< FromJsonTests::UnquotedFieldNameBad13 >();
            add< FromJsonTests::UnquotedFieldNameBad14 >();
            add< FromJsonTests::UnquotedFieldNameBad15 >();
            add< FromJsonTests::UnquotedFieldNameBad16 >();
            add< FromJsonTests::UnquotedFieldNameBad17 >();
            add< FromJsonTests::UnquotedFieldNameBad18 >();
            add< FromJsonTests::UnquotedFieldNameBad19 >();
            add< FromJsonTests::UnquotedFieldNameBad20 >();
            add< FromJsonTests::UnquotedFieldNameDollar >();
            add< FromJsonTests::SingleQuotes >();
            add< FromJsonTests::QuoteTest >();
            add< FromJsonTests::QuoteTest1 >();
            add< FromJsonTests::QuoteTest2 >();
            add< FromJsonTests::QuoteTest3 >();
            add< FromJsonTests::QuoteTest4 >();
            add< FromJsonTests::QuoteTest5 >();
            add< FromJsonTests::QuoteTest6 >();
            add< FromJsonTests::ObjectId >();
            add< FromJsonTests::ObjectId2 >();
            add< FromJsonTests::NumericTypes >();
            add< FromJsonTests::NumericLimits >();
            add< FromJsonTests::NumericLimitsBad >();
            add< FromJsonTests::NumericLimitsBad1 >();
            add< FromJsonTests::NegativeNumericTypes >();
            add< FromJsonTests::EmbeddedDatesFormat1 >();
            add< FromJsonTests::EmbeddedDatesFormat2 >();
            add< FromJsonTests::EmbeddedDatesFormat3 >();
            add< FromJsonTests::NullString >();
            add< FromJsonTests::NullFieldUnquoted >();
        }
    } myall;

} // namespace JsonTests

