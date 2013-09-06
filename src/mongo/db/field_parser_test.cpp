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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include <string>
#include <vector>
#include <map>

#include "mongo/bson/bson_field.h"
#include "mongo/db/field_parser.h"
#include "mongo/db/jsobj.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"

namespace {

    using mongo::BSONArray;
    using mongo::BSONField;
    using mongo::BSONObj;
    using mongo::BSONObjBuilder;
    using mongo::Date_t;
    using mongo::FieldParser;
    using mongo::OID;
    using std::string;
    using std::vector;
    using std::map;

    class ExtractionFixture: public mongo::unittest::Test {
    protected:
        BSONObj doc;

        bool valBool;
        BSONArray valArray;
        BSONObj valObj;
        Date_t valDate;
        string valString;
        OID valOID;
        long long valLong;

        static BSONField<bool> aBool;
        static BSONField<BSONArray> anArray;
        static BSONField<BSONObj> anObj;
        static BSONField<Date_t> aDate;
        static BSONField<string> aString;
        static BSONField<OID> anOID;
        static BSONField<long long> aLong;

        void setUp() {
            valBool = true;
            valArray = BSON_ARRAY(1 << 2 << 3);
            valObj = BSON("a" << 1);
            valDate = 1ULL;
            valString = "a string";
            valOID = OID::gen();
            valLong = 1LL;

            doc = BSON(aBool(valBool) <<
                    anArray(valArray) <<
                    anObj(valObj) <<
                    aDate(valDate) <<
                    aString(valString) <<
                    anOID(valOID) <<
                    aLong(valLong));
        }

        void tearDown() {
        }
    };

    BSONField<bool> ExtractionFixture::aBool("aBool");
    BSONField<BSONArray> ExtractionFixture::anArray("anArray");
    BSONField<BSONObj> ExtractionFixture::anObj("anObj");
    BSONField<Date_t> ExtractionFixture::aDate("aDate");
    BSONField<string> ExtractionFixture::aString("aString");
    BSONField<OID> ExtractionFixture::anOID("anOID");
    BSONField<long long> ExtractionFixture::aLong("aLong");

    TEST_F(ExtractionFixture, GetBool) {
        BSONField<bool> notThere("otherBool", true);
        BSONField<bool> wrongType(anObj.name());
        bool val;
        ASSERT_TRUE(FieldParser::extract(doc, aBool, &val));
        ASSERT_EQUALS(val, valBool);
        ASSERT_TRUE(FieldParser::extract(doc, notThere, &val));
        ASSERT_EQUALS(val, true);
        ASSERT_FALSE(FieldParser::extract(doc, wrongType, &val));
    }

    TEST_F(ExtractionFixture, GetBSONArray) {
        BSONField<BSONArray> notThere("otherArray", BSON_ARRAY("a" << "b"));
        BSONField<BSONArray> wrongType(aString.name());
        BSONArray val;
        ASSERT_TRUE(FieldParser::extract(doc, anArray, &val));
        ASSERT_EQUALS(val, valArray);
        ASSERT_TRUE(FieldParser::extract(doc, notThere, &val));
        ASSERT_EQUALS(val, BSON_ARRAY("a" << "b"));
        ASSERT_FALSE(FieldParser::extract(doc, wrongType, &val));
    }

    TEST_F(ExtractionFixture, GetBSONObj) {
        BSONField<BSONObj> notThere("otherObj", BSON("b" << 1));
        BSONField<BSONObj> wrongType(aString.name());
        BSONObj val;
        ASSERT_TRUE(FieldParser::extract(doc, anObj, &val));
        ASSERT_EQUALS(val, valObj);
        ASSERT_TRUE(FieldParser::extract(doc, notThere, &val));
        ASSERT_EQUALS(val, BSON("b" << 1));
        ASSERT_FALSE(FieldParser::extract(doc, wrongType, &val));
    }

    TEST_F(ExtractionFixture, GetDate) {
        BSONField<Date_t> notThere("otherDate", 99ULL);
        BSONField<Date_t> wrongType(aString.name());
        Date_t val;
        ASSERT_TRUE(FieldParser::extract(doc, aDate, &val));
        ASSERT_EQUALS(val, valDate);
        ASSERT_TRUE(FieldParser::extract(doc, notThere, &val));
        ASSERT_EQUALS(val, 99ULL);
        ASSERT_FALSE(FieldParser::extract(doc, wrongType, &val));
    }

    TEST_F(ExtractionFixture, GetString) {
        BSONField<string> notThere("otherString", "abc");
        BSONField<string> wrongType(aBool.name());
        string val;
        ASSERT_TRUE(FieldParser::extract(doc, aString, &val));
        ASSERT_EQUALS(val, valString);
        ASSERT_TRUE(FieldParser::extract(doc, notThere, &val));
        ASSERT_EQUALS(val, "abc");
        ASSERT_FALSE(FieldParser::extract(doc, wrongType, &val));
    }

    TEST_F(ExtractionFixture, GetOID) {
        OID defOID = OID::gen();
        BSONField<OID> notThere("otherOID", defOID);
        BSONField<OID> wrongType(aString.name());
        OID val;
        ASSERT_TRUE(FieldParser::extract(doc, anOID, &val));
        ASSERT_EQUALS(val, valOID);
        ASSERT_TRUE(FieldParser::extract(doc, notThere, &val));
        ASSERT_EQUALS(val, defOID);
        ASSERT_FALSE(FieldParser::extract(doc, wrongType, &val));
    }

    TEST_F(ExtractionFixture, GetLong) {
        BSONField<long long> notThere("otherLong", 0);
        BSONField<long long> wrongType(aString.name());
        long long val;
        ASSERT_TRUE(FieldParser::extract(doc, aLong, &val));
        ASSERT_EQUALS(val, valLong);
        ASSERT_TRUE(FieldParser::extract(doc, notThere, &val));
        ASSERT_EQUALS(val, 0);
        ASSERT_FALSE(FieldParser::extract(doc, wrongType, &val));
    }

    TEST_F(ExtractionFixture, IsFound) {
        bool bool_val;
        BSONField<bool> aBoolMissing("aBoolMissing");
        ASSERT_EQUALS(FieldParser::extract(doc, aBool, &bool_val, NULL),
                      FieldParser::FIELD_SET);
        ASSERT_EQUALS(FieldParser::extract(doc, aBoolMissing, &bool_val, NULL),
                      FieldParser::FIELD_NONE);

        Date_t Date_t_val;
        BSONField<Date_t> aDateMissing("aDateMissing");
        ASSERT_EQUALS(FieldParser::extract(doc, aDate, &Date_t_val, NULL),
                      FieldParser::FIELD_SET);
        ASSERT_EQUALS(FieldParser::extract(doc, aDateMissing, &Date_t_val, NULL),
                      FieldParser::FIELD_NONE);

        string string_val;
        BSONField<string> aStringMissing("aStringMissing");
        ASSERT_EQUALS(FieldParser::extract(doc, aString, &string_val, NULL),
                      FieldParser::FIELD_SET);
        ASSERT_EQUALS(FieldParser::extract(doc, aStringMissing, &string_val, NULL),
                      FieldParser::FIELD_NONE);

        OID OID_val;
        BSONField<OID> anOIDMissing("anOIDMissing");
        ASSERT_EQUALS(FieldParser::extract(doc, anOID, &OID_val, NULL),
                      FieldParser::FIELD_SET);
        ASSERT_EQUALS(FieldParser::extract(doc, anOIDMissing, &OID_val, NULL),
                      FieldParser::FIELD_NONE);

        long long long_long_val;
        BSONField<long long> aLongMissing("aLongMissing");
        ASSERT_EQUALS(FieldParser::extract(doc, aLong, &long_long_val, NULL),
                      FieldParser::FIELD_SET);
        ASSERT_EQUALS(FieldParser::extract(doc, aLongMissing, &long_long_val, NULL),
                      FieldParser::FIELD_NONE);
    }

    TEST(ComplexExtraction, GetStringVector) {

        // Test valid string vector extraction
        BSONField<vector<string> > vectorField("testVector");

        BSONObjBuilder bob;
        bob << vectorField() << BSON_ARRAY("a" << "b" << "c");
        BSONObj obj = bob.obj();

        vector<string> parsedVector;

        ASSERT(FieldParser::extract(obj, vectorField, &parsedVector));
        ASSERT_EQUALS("a", parsedVector[0]);
        ASSERT_EQUALS("b", parsedVector[1]);
        ASSERT_EQUALS("c", parsedVector[2]);
        ASSERT_EQUALS(parsedVector.size(), static_cast<size_t>(3));
    }

    TEST(ComplexExtraction, GetObjectVector) {

        // Test valid BSONObj vector extraction
        BSONField<vector<BSONObj> > vectorField("testVector");

        BSONObjBuilder bob;
        bob << vectorField() << BSON_ARRAY(BSON("a" << 1) << BSON("b" << 1) << BSON("c" << 1));
        BSONObj obj = bob.obj();

        vector<BSONObj> parsedVector;

        ASSERT(FieldParser::extract(obj, vectorField, &parsedVector));
        ASSERT_EQUALS(BSON("a" << 1), parsedVector[0]);
        ASSERT_EQUALS(BSON("b" << 1), parsedVector[1]);
        ASSERT_EQUALS(BSON("c" << 1), parsedVector[2]);
        ASSERT_EQUALS(parsedVector.size(), static_cast<size_t>(3));
    }

    TEST(ComplexExtraction, GetBadVector) {

        // Test invalid vector extraction
        BSONField<vector<BSONObj> > vectorField("testVector");

        BSONObjBuilder bob;
        bob << vectorField() << BSON_ARRAY(BSON("a" << 1) << "XXX" << BSON("c" << 1));
        BSONObj obj = bob.obj();

        vector<BSONObj> parsedVector;

        string errMsg;
        ASSERT(!FieldParser::extract(obj, vectorField, &parsedVector, &errMsg));
        ASSERT_NOT_EQUALS(errMsg, "");
    }

    TEST(ComplexExtraction, RoundTripVector) {

        // Test vector extraction after re-writing to BSON
        BSONField<vector<string> > vectorField("testVector");

        BSONObj obj;
        {
            BSONObjBuilder bob;
            bob << vectorField() << BSON_ARRAY("a" << "b" << "c");
            obj = bob.obj();
        }

        vector<string> parsedVector;
        ASSERT(FieldParser::extract(obj, vectorField, &parsedVector));

        {
            BSONObjBuilder bob;
            bob.append(vectorField(), parsedVector);
            obj = bob.obj();
        }

        parsedVector.clear();
        ASSERT(FieldParser::extract(obj, vectorField, &parsedVector));

        ASSERT_EQUALS("a", parsedVector[0]);
        ASSERT_EQUALS("b", parsedVector[1]);
        ASSERT_EQUALS("c", parsedVector[2]);
        ASSERT_EQUALS(parsedVector.size(), static_cast<size_t>(3));
    }

    TEST(ComplexExtraction, GetStringMap) {

        // Test valid string->string map extraction
        BSONField<map<string, string> > mapField("testMap");

        BSONObjBuilder bob;
        bob << mapField() << BSON("a" << "a" << "b" << "b" << "c" << "c");
        BSONObj obj = bob.obj();

        map<string, string> parsedMap;

        ASSERT(FieldParser::extract(obj, mapField, &parsedMap));
        ASSERT_EQUALS("a", parsedMap["a"]);
        ASSERT_EQUALS("b", parsedMap["b"]);
        ASSERT_EQUALS("c", parsedMap["c"]);
        ASSERT_EQUALS(parsedMap.size(), static_cast<size_t>(3));
    }

    TEST(ComplexExtraction, GetObjectMap) {

        // Test valid string->BSONObj map extraction
        BSONField<map<string, BSONObj> > mapField("testMap");

        BSONObjBuilder bob;
        bob << mapField() << BSON("a" << BSON("a" << "a") <<
                "b" << BSON("b" << "b") <<
                "c" << BSON("c" << "c"));
        BSONObj obj = bob.obj();

        map<string, BSONObj> parsedMap;

        ASSERT(FieldParser::extract(obj, mapField, &parsedMap));
        ASSERT_EQUALS(BSON("a" << "a"), parsedMap["a"]);
        ASSERT_EQUALS(BSON("b" << "b"), parsedMap["b"]);
        ASSERT_EQUALS(BSON("c" << "c"), parsedMap["c"]);
        ASSERT_EQUALS(parsedMap.size(), static_cast<size_t>(3));
    }

    TEST(ComplexExtraction, GetBadMap) {

        // Test invalid map extraction
        BSONField<map<string, string> > mapField("testMap");

        BSONObjBuilder bob;
        bob << mapField() << BSON("a" << "a" << "b" << 123 << "c" << "c");
        BSONObj obj = bob.obj();

        map<string, string> parsedMap;

        string errMsg;
        ASSERT(!FieldParser::extract(obj, mapField, &parsedMap, &errMsg));
        ASSERT_NOT_EQUALS(errMsg, "");
    }

    TEST(ComplexExtraction, RoundTripMap) {

        // Test map extraction after re-writing to BSON
        BSONField<map<string, string> > mapField("testMap");

        BSONObj obj;
        {
            BSONObjBuilder bob;
            bob << mapField() << BSON("a" << "a" << "b" << "b" << "c" << "c");
            obj = bob.obj();
        }

        map<string, string> parsedMap;
        ASSERT(FieldParser::extract(obj, mapField, &parsedMap));

        {
            BSONObjBuilder bob;
            bob.append(mapField(), parsedMap);
            obj = bob.obj();
        }

        parsedMap.clear();
        ASSERT(FieldParser::extract(obj, mapField, &parsedMap));

        ASSERT_EQUALS("a", parsedMap["a"]);
        ASSERT_EQUALS("b", parsedMap["b"]);
        ASSERT_EQUALS("c", parsedMap["c"]);
        ASSERT_EQUALS(parsedMap.size(), static_cast<size_t>(3));
    }

    TEST(ComplexExtraction, GetNestedMap) {

        // Test extraction of complex nested vector and map
        BSONField<vector<map<string, string> > > nestedField("testNested");

        BSONObj nestedMapObj = BSON("a" << "a" << "b" << "b" << "c" << "c");

        BSONObjBuilder bob;
        bob << nestedField() << BSON_ARRAY(nestedMapObj << nestedMapObj << nestedMapObj);
        BSONObj obj = bob.obj();

        vector<map<string, string> > parsed;

        ASSERT(FieldParser::extract(obj, nestedField, &parsed));
        ASSERT_EQUALS(parsed.size(), static_cast<size_t>(3));
        for (int i = 0; i < 3; i++) {
            map<string, string>& parsedMap = parsed[i];
            ASSERT_EQUALS("a", parsedMap["a"]);
            ASSERT_EQUALS("b", parsedMap["b"]);
            ASSERT_EQUALS("c", parsedMap["c"]);
            ASSERT_EQUALS(parsedMap.size(), static_cast<size_t>(3));
        }
    }

    TEST(ComplexExtraction, GetBadNestedMap) {

        // Test extraction of invalid complex nested vector and map
        BSONField<vector<map<string, string> > > nestedField("testNested");

        BSONObj nestedMapObj = BSON("a" << "a" << "b" << 123 << "c" << "c");

        BSONObjBuilder bob;
        bob << nestedField() << BSON_ARRAY(nestedMapObj << nestedMapObj << nestedMapObj);
        BSONObj obj = bob.obj();

        vector<map<string, string> > parsed;

        string errMsg;
        ASSERT(!FieldParser::extract(obj, nestedField, &parsed, &errMsg));
        ASSERT_NOT_EQUALS(errMsg, "");
    }

} // unnamed namespace
