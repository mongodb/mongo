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

#include <string>

#include "mongo/bson/bson_field.h"
#include "mongo/bson/util/misc.h" // for Date_t
#include "mongo/db/jsobj.h"
#include "mongo/s/field_parser.h"
#include "mongo/unittest/unittest.h"

namespace {

    using mongo::BSONArray;
    using mongo::BSONField;
    using mongo::BSONObj;
    using mongo::Date_t;
    using mongo::FieldParser;
    using mongo::OID;
    using std::string;

    class ExtractionFixture : public mongo::unittest::Test {
    protected:
        BSONObj doc;

        bool valBool;
        BSONArray valArray;
        BSONObj valObj;
        Date_t valDate;
        string valString;
        OID valOID;

        static BSONField<bool> aBool;
        static BSONField<BSONArray> anArray;
        static BSONField<BSONObj> anObj;
        static BSONField<Date_t> aDate;
        static BSONField<string> aString;
        static BSONField<OID> anOID;

        void setUp() {
            valBool = true;
            valArray = BSON_ARRAY(1 << 2 << 3);
            valObj = BSON("a" << 1);
            valDate = 1ULL;
            valString = "a string";
            valOID = OID::gen();

            doc = BSON(aBool(valBool) <<
                       anArray(valArray) <<
                       anObj(valObj) <<
                       aDate(valDate) <<
                       aString(valString) <<
                       anOID(valOID));
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

    TEST_F(ExtractionFixture, GetBool) {
        BSONField<bool> notThere("otherBool");
        BSONField<bool> wrongType(anObj.name());
        bool val;
        ASSERT_TRUE(FieldParser::extract(doc, aBool, false, &val));
        ASSERT_EQUALS(val, valBool);
        ASSERT_TRUE(FieldParser::extract(doc, notThere, true, &val));
        ASSERT_EQUALS(val, true);
        ASSERT_FALSE(FieldParser::extract(doc, wrongType, true, &val));
    }

    TEST_F(ExtractionFixture, GetBSONArray) {
        BSONField<BSONArray> notThere("otherArray");
        BSONField<BSONArray> wrongType(aString.name());
        BSONArray val;
        ASSERT_TRUE(FieldParser::extract(doc, anArray, BSONArray(), &val));
        ASSERT_EQUALS(val, valArray);
        ASSERT_TRUE(FieldParser::extract(doc, notThere, BSON_ARRAY("a" << "b"), &val));
        ASSERT_EQUALS(val, BSON_ARRAY("a" << "b"));
        ASSERT_FALSE(FieldParser::extract(doc, wrongType, BSON_ARRAY("a" << "b"), &val));
    }

    TEST_F(ExtractionFixture, GetBSONObj) {
        BSONField<BSONObj> notThere("otherObj");
        BSONField<BSONObj> wrongType(aString.name());
        BSONObj val;
        ASSERT_TRUE(FieldParser::extract(doc, anObj, BSONObj(), &val));
        ASSERT_EQUALS(val, valObj);
        ASSERT_TRUE(FieldParser::extract(doc, notThere, BSON("b" << 1), &val));
        ASSERT_EQUALS(val, BSON("b" << 1));
        ASSERT_FALSE(FieldParser::extract(doc, wrongType, BSON("b" << 1), &val));
    }

    TEST_F(ExtractionFixture, GetDate) {
        BSONField<Date_t> notThere("otherDate");
        BSONField<Date_t> wrongType(aString.name());
        Date_t val;
        ASSERT_TRUE(FieldParser::extract(doc, aDate, time(0), &val));
        ASSERT_EQUALS(val, valDate);
        ASSERT_TRUE(FieldParser::extract(doc, notThere, 99ULL, &val));
        ASSERT_EQUALS(val, 99ULL);
        ASSERT_FALSE(FieldParser::extract(doc, wrongType, 99ULL, &val));
    }

    TEST_F(ExtractionFixture, GetString) {
        BSONField<string> notThere("otherString");
        BSONField<string> wrongType(aBool.name());
        string val;
        ASSERT_TRUE(FieldParser::extract(doc, aString, "", &val));
        ASSERT_EQUALS(val, valString);
        ASSERT_TRUE(FieldParser::extract(doc, notThere, "abc", &val));
        ASSERT_EQUALS(val, "abc");
        ASSERT_FALSE(FieldParser::extract(doc, wrongType, "abc", &val));
    }

    TEST_F(ExtractionFixture, GetOID) {
        BSONField<OID> notThere("otherOID");
        BSONField<OID> wrongType(aString.name());
        OID defOID = OID::gen();
        OID val;
        ASSERT_TRUE(FieldParser::extract(doc, anOID, OID(), &val));
        ASSERT_EQUALS(val, valOID);
        ASSERT_TRUE(FieldParser::extract(doc, notThere, defOID, &val));
        ASSERT_EQUALS(val, defOID);
        ASSERT_FALSE(FieldParser::extract(doc, wrongType, defOID, &val));
    }

} // unnamed namespace
