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

    class ExtractionFixture : public mongo::unittest::Test {
    protected:
        mongo::BSONObj doc;

        bool valBool;
        mongo::BSONObj valObj;
        mongo::Date_t valDate;
        std::string valString;
        mongo::OID valOID;

        static mongo::BSONField<bool> aBool;
        static mongo::BSONField<mongo::BSONObj> anObj;
        static mongo::BSONField<mongo::Date_t> aDate;
        static mongo::BSONField<std::string> aString;
        static mongo::BSONField<mongo::OID> anOID;

        void setUp() {
            valBool = true;
            valObj = BSON("a" << 1);
            valDate = 1ULL;
            valString = "a string";
            valOID = mongo::OID::gen();

            doc = BSON(aBool(valBool) <<
                       anObj(valObj) <<
                       aDate(valDate) <<
                       aString(valString) <<
                       anOID(valOID));
        }

        void tearDown() {
        }
    };

    mongo::BSONField<bool> ExtractionFixture::aBool("aBool");
    mongo::BSONField<mongo::BSONObj> ExtractionFixture::anObj("anObj");
    mongo::BSONField<mongo::Date_t> ExtractionFixture::aDate("aDate");
    mongo::BSONField<std::string> ExtractionFixture::aString("aString");
    mongo::BSONField<mongo::OID> ExtractionFixture::anOID("anOID");

    TEST_F(ExtractionFixture, GetBool) {
        mongo::BSONField<bool> notThere("otherBool");
        mongo::BSONField<bool> wrongType(aString.name());
        bool val;
        ASSERT_TRUE(mongo::FieldParser::extract(doc, aBool, false, &val));
        ASSERT_EQUALS(val, valBool);
        ASSERT_TRUE(mongo::FieldParser::extract(doc, notThere, true, &val));
        ASSERT_EQUALS(val, true);
        ASSERT_FALSE(mongo::FieldParser::extract(doc, wrongType, true, &val));
    }

    TEST_F(ExtractionFixture, GetBSONObj) {
        mongo::BSONField<mongo::BSONObj> notThere("otherObj");
        mongo::BSONField<mongo::BSONObj> wrongType(aString.name());
        mongo::BSONObj val;
        ASSERT_TRUE(mongo::FieldParser::extract(doc, anObj, mongo::BSONObj(), &val));
        ASSERT_EQUALS(val, valObj);
        ASSERT_TRUE(mongo::FieldParser::extract(doc, notThere, BSON("b" << 1), &val));
        ASSERT_EQUALS(val, BSON("b" << 1));
        ASSERT_FALSE(mongo::FieldParser::extract(doc, wrongType, BSON("b" << 1), &val));
    }

    TEST_F(ExtractionFixture, GetDate) {
        mongo::BSONField<mongo::Date_t> notThere("otherDate");
        mongo::BSONField<mongo::Date_t> wrongType(aString.name());
        mongo::Date_t val;
        ASSERT_TRUE(mongo::FieldParser::extract(doc, aDate, time(0), &val));
        ASSERT_EQUALS(val, valDate);
        ASSERT_TRUE(mongo::FieldParser::extract(doc, notThere, 99ULL, &val));
        ASSERT_EQUALS(val, 99ULL);
        ASSERT_FALSE(mongo::FieldParser::extract(doc, wrongType, 99ULL, &val));
    }

    TEST_F(ExtractionFixture, GetString) {
        mongo::BSONField<std::string> notThere("otherString");
        mongo::BSONField<std::string> wrongType(aBool.name());
        std::string val;
        ASSERT_TRUE(mongo::FieldParser::extract(doc, aString, "", &val));
        ASSERT_EQUALS(val, valString);
        ASSERT_TRUE(mongo::FieldParser::extract(doc, notThere, "abc", &val));
        ASSERT_EQUALS(val, "abc");
        ASSERT_FALSE(mongo::FieldParser::extract(doc, wrongType, "abc", &val));
    }

    TEST_F(ExtractionFixture, GetOID) {
        mongo::BSONField<mongo::OID> notThere("otherOID");
        mongo::BSONField<mongo::OID> wrongType(aString.name());
        mongo::OID defOID = mongo::OID::gen();
        mongo::OID val;
        ASSERT_TRUE(mongo::FieldParser::extract(doc, anOID, mongo::OID(), &val));
        ASSERT_EQUALS(val, valOID);
        ASSERT_TRUE(mongo::FieldParser::extract(doc, notThere, defOID, &val));
        ASSERT_EQUALS(val, defOID);
        ASSERT_FALSE(mongo::FieldParser::extract(doc, wrongType, defOID, &val));
    }

} // unnamed namespace
