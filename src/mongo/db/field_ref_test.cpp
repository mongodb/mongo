/*    Copyright 2012 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include <string>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/db/field_ref.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/mongoutils/str.h"

namespace {

    using mongo::FieldRef;
    using mongo::StringData;
    using mongoutils::str::stream;
    using std::string;

    TEST(Empty, NoFields) {
        FieldRef fieldRef;
        fieldRef.parse("");
        ASSERT_EQUALS(fieldRef.numParts(), 0U);
        ASSERT_EQUALS(fieldRef.dottedField(), "");
    }

    TEST(Empty, NoFieldNames) {
        string field = ".";
        FieldRef fieldRef;
        fieldRef.parse(field);
        ASSERT_EQUALS(fieldRef.numParts(), 2U);
        ASSERT_EQUALS(fieldRef.getPart(0), "");
        ASSERT_EQUALS(fieldRef.getPart(1), "");
        ASSERT_EQUALS(fieldRef.dottedField(), field);
    }

    TEST(Empty, NoFieldNames2) {
        string field = "..";
        FieldRef fieldRef;
        fieldRef.parse(field);
        ASSERT_EQUALS(fieldRef.numParts(), 3U);
        ASSERT_EQUALS(fieldRef.getPart(0), "");
        ASSERT_EQUALS(fieldRef.getPart(1), "");
        ASSERT_EQUALS(fieldRef.getPart(2), "");
        ASSERT_EQUALS(fieldRef.dottedField(), field);
    }

    TEST(Empty, EmptyFieldName) {
        string field = ".b.";
        FieldRef fieldRef;
        fieldRef.parse(field);
        ASSERT_EQUALS(fieldRef.numParts(), 3U);
        ASSERT_EQUALS(fieldRef.getPart(0), "");
        ASSERT_EQUALS(fieldRef.getPart(1), "b");
        ASSERT_EQUALS(fieldRef.getPart(2), "");
        ASSERT_EQUALS(fieldRef.dottedField(), field);
    }

    TEST(Normal, SinglePart) {
        string field = "a";
        FieldRef fieldRef;
        fieldRef.parse(field);
        ASSERT_EQUALS(fieldRef.numParts(), 1U);
        ASSERT_EQUALS(fieldRef.getPart(0), field);
        ASSERT_EQUALS(fieldRef.dottedField(), field);
    }

    TEST(Normal, ParseTwice) {
        string field = "a";
        FieldRef fieldRef;
        for (int i = 0; i < 2; i++) {
            fieldRef.parse(field);
            ASSERT_EQUALS(fieldRef.numParts(), 1U);
            ASSERT_EQUALS(fieldRef.getPart(0), field);
            ASSERT_EQUALS(fieldRef.dottedField(), field);
        }
    }

    TEST(Normal, MulitplePartsVariable) {
        const char* parts[] = {"a", "b", "c", "d", "e"};
        size_t size = sizeof(parts)/sizeof(char*);
        string field(parts[0]);
        for (size_t i=1; i<size; i++) {
            field.append(1, '.');
            field.append(parts[i]);
        }

        FieldRef fieldRef;
        fieldRef.parse(field);
        ASSERT_EQUALS(fieldRef.numParts(), size);
        for (size_t i=0; i<size; i++) {
            ASSERT_EQUALS(fieldRef.getPart(i), parts[i]);
        }
        ASSERT_EQUALS(fieldRef.dottedField(), field);
    }

    TEST(Replacement, SingleField) {
        string field = "$";
        FieldRef fieldRef;
        fieldRef.parse(field);
        ASSERT_EQUALS(fieldRef.numParts(), 1U);
        ASSERT_EQUALS(fieldRef.getPart(0), "$");

        string newField = "a";
        fieldRef.setPart(0, newField);
        ASSERT_EQUALS(fieldRef.numParts(), 1U);
        ASSERT_EQUALS(fieldRef.getPart(0), newField);
        ASSERT_EQUALS(fieldRef.dottedField(), newField);
    }

    TEST(Replacement, InMultipleField) {
        string field = "a.b.c.$.e";
        FieldRef fieldRef;
        fieldRef.parse(field);
        ASSERT_EQUALS(fieldRef.numParts(), 5U);
        ASSERT_EQUALS(fieldRef.getPart(3), "$");

        string newField = "d";
        fieldRef.setPart(3, newField);
        ASSERT_EQUALS(fieldRef.numParts(), 5U);
        ASSERT_EQUALS(fieldRef.getPart(3), newField);
        ASSERT_EQUALS(fieldRef.dottedField(), "a.b.c.d.e");
    }

    TEST(Replacement, SameFieldMultipleReplacements) {
        string prefix = "a.";
        string field = prefix + "$";
        FieldRef fieldRef;
        fieldRef.parse(field);
        ASSERT_EQUALS(fieldRef.numParts(), 2U);

        const char* parts[] = {"a", "b", "c", "d", "e"};
        size_t size = sizeof(parts)/sizeof(char*);
        for (size_t i=0; i<size; i++) {
            fieldRef.setPart(1, parts[i]);
            ASSERT_EQUALS(fieldRef.dottedField(), prefix + parts[i]);
        }
        ASSERT_EQUALS(fieldRef.numReplaced(), 1U);
    }

    TEST( Prefix, Normal ) {
        FieldRef prefix, base;
        base.parse( "a.b.c" );

        prefix.parse( "a.b" );
        ASSERT_TRUE( prefix.isPrefixOf( base ) );

        prefix.parse( "a" );
        ASSERT_TRUE( prefix.isPrefixOf( base ) );
    }

    TEST( Prefix, Dotted ) {
        FieldRef prefix, base;
        base.parse( "a.0.c" );
        prefix.parse( "a.0" );
        ASSERT_TRUE( prefix.isPrefixOf( base ) );
    }

    TEST( Prefix, NoPrefixes ) {
        FieldRef prefix, base;
        prefix.parse( "a.b" );
        base.parse( "a.b" );
        ASSERT_FALSE( prefix.isPrefixOf( base ) );

        base.parse( "a" );
        ASSERT_FALSE( prefix.isPrefixOf( base ) );

        base.parse( "b" );
        ASSERT_FALSE( prefix.isPrefixOf( base ) );
    }

    TEST( Prefix, EmptyBase ) {
        FieldRef field, empty;
        field.parse( "a" );
        ASSERT_FALSE( field.isPrefixOf( empty ) );
        ASSERT_FALSE( empty.isPrefixOf( field ) );
        ASSERT_FALSE( empty.isPrefixOf( empty ) );
    }

    TEST( PrefixSize, Normal ) {
        FieldRef fieldA, fieldB;
        fieldA.parse( "a.b" );

        fieldB.parse( "a" );
        ASSERT_EQUALS( fieldA.commonPrefixSize( fieldB ), 1U );

        fieldB.parse( "a.b" );
        ASSERT_EQUALS( fieldA.commonPrefixSize( fieldB ), 2U );

        fieldB.parse( "a.b.c" );
        ASSERT_EQUALS( fieldA.commonPrefixSize( fieldB ), 2U );
    }

    TEST( PrefixSize, NoCommonatility ) {
        FieldRef fieldA, fieldB;
        fieldA.parse( "a" );
        fieldB.parse( "b" );
        ASSERT_EQUALS( fieldA.commonPrefixSize( fieldB ), 0U );
    }

    TEST( PrefixSize, Empty ) {
        FieldRef fieldA, empty;
        fieldA.parse( "a" );
        ASSERT_EQUALS( fieldA.commonPrefixSize( empty ), 0U );
        ASSERT_EQUALS( empty.commonPrefixSize( fieldA ), 0U );
    }

    TEST( Equality, Simple1 ) {
        FieldRef a;
        a.parse( "a.b" );
        ASSERT( a.equalsDottedField( "a.b" ) );
        ASSERT( !a.equalsDottedField( "a" ) );
        ASSERT( !a.equalsDottedField( "b" ) );
        ASSERT( !a.equalsDottedField( "a.b.c" ) );
    }

    TEST( Equality, Simple2 ) {
        FieldRef a;
        a.parse( "a" );
        ASSERT( !a.equalsDottedField( "a.b" ) );
        ASSERT( a.equalsDottedField( "a" ) );
        ASSERT( !a.equalsDottedField( "b" ) );
        ASSERT( !a.equalsDottedField( "a.b.c" ) );
    }

    TEST( Comparison, BothEmpty ) {
        FieldRef a;
        ASSERT_TRUE( a == a );
        ASSERT_FALSE( a != a );
        ASSERT_FALSE( a < a );
        ASSERT_TRUE( a <= a );
        ASSERT_FALSE( a > a );
        ASSERT_TRUE( a >= a );
    }

    TEST( Comparison, EqualInSize ) {
        FieldRef a, b;
        a.parse( "a.b.c" );
        b.parse( "a.d.c" );
        ASSERT_FALSE( a == b );
        ASSERT_TRUE( a != b );
        ASSERT_TRUE( a < b );
        ASSERT_TRUE( a <= b );
        ASSERT_FALSE( a > b );
        ASSERT_FALSE( a >= b );
    }

    TEST( Comparison, NonEqual ) {
        FieldRef a, b;
        a.parse( "a.b.c" );
        b.parse( "b.d" );
        ASSERT_FALSE( a == b );
        ASSERT_TRUE( a != b );
        ASSERT_TRUE( a < b );
        ASSERT_TRUE( a <= b );
        ASSERT_FALSE( a > b );
        ASSERT_FALSE( a >= b );
    }

    TEST( Comparison, MixedEmtpyAndNot ) {
        FieldRef a, b;
        a.parse( "a" );
        ASSERT_FALSE( a == b );
        ASSERT_TRUE( a != b );
        ASSERT_FALSE( a < b );
        ASSERT_FALSE( a <= b );
        ASSERT_TRUE( a > b );
        ASSERT_TRUE( a >= b );
    }

    TEST( DottedField, Simple1 ) {
        FieldRef a;
        a.parse( "a.b.c.d.e" );
        ASSERT_EQUALS( "a.b.c.d.e", a.dottedField() );
        ASSERT_EQUALS( "a.b.c.d.e", a.dottedField(0) );
        ASSERT_EQUALS( "b.c.d.e", a.dottedField(1) );
        ASSERT_EQUALS( "c.d.e", a.dottedField(2) );
        ASSERT_EQUALS( "d.e", a.dottedField(3) );
        ASSERT_EQUALS( "e", a.dottedField(4) );
        ASSERT_EQUALS( "", a.dottedField(5) );
        ASSERT_EQUALS( "", a.dottedField(6) );
    }

} // namespace
