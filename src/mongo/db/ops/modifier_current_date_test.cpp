/**
 *    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/ops/modifier_current_date.h"

#include <cstdint>

#include "mongo/base/string_data.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/mutable_bson_test_utils.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/ops/log_builder.h"
#include "mongo/unittest/unittest.h"

namespace {

using mongo::BSONObj;
using mongo::LogBuilder;
using mongo::ModifierCurrentDate;
using mongo::ModifierInterface;
using mongo::Timestamp;
using mongo::Status;
using mongo::StringData;
using mongo::fromjson;
using mongo::mutablebson::ConstElement;
using mongo::mutablebson::Document;
using mongo::mutablebson::Element;

/**
 * Helper to validate oplog entries in the tests below.
 */
void validateOplogEntry(BSONObj& oplogFormat, Document& doc) {
    // Ensure that the field is the same
    ASSERT_EQUALS(oplogFormat.firstElement().fieldName(), doc.root().leftChild().getFieldName());

    // Ensure the field names are the same
    ASSERT_EQUALS(oplogFormat.firstElement().embeddedObject().firstElement().fieldName(),
                  doc.root().leftChild().leftChild().getFieldName());

    // Ensure the type is the same in the document as the oplog
    ASSERT_EQUALS(oplogFormat.firstElement().embeddedObject().firstElement().type(),
                  doc.root().leftChild().leftChild().getType());
}

/** Helper to build and manipulate a $currentDate mod. */
class Mod {
public:
    Mod() : _mod() {}

    explicit Mod(BSONObj modObj) : _modObj(modObj), _mod() {
        ASSERT_OK(_mod.init(_modObj["$currentDate"].embeddedObject().firstElement(),
                            ModifierInterface::Options::normal()));
    }

    Status prepare(Element root, StringData matchedField, ModifierInterface::ExecInfo* execInfo) {
        return _mod.prepare(root, matchedField, execInfo);
    }

    Status apply() const {
        return _mod.apply();
    }

    Status log(LogBuilder* logBuilder) const {
        return _mod.log(logBuilder);
    }

    ModifierCurrentDate& mod() {
        return _mod;
    }

private:
    BSONObj _modObj;
    ModifierCurrentDate _mod;
};

TEST(Init, ValidValues) {
    BSONObj modObj;
    ModifierCurrentDate mod;

    modObj = fromjson("{ $currentDate : { a : true } }");
    ASSERT_OK(mod.init(modObj["$currentDate"].embeddedObject().firstElement(),
                       ModifierInterface::Options::normal()));

    modObj = fromjson("{ $currentDate : { a : {$type : 'timestamp' } } }");
    ASSERT_OK(mod.init(modObj["$currentDate"].embeddedObject().firstElement(),
                       ModifierInterface::Options::normal()));

    modObj = fromjson("{ $currentDate : { a : {$type : 'date' } } }");
    ASSERT_OK(mod.init(modObj["$currentDate"].embeddedObject().firstElement(),
                       ModifierInterface::Options::normal()));
}

TEST(Init, FailToInitWithInvalidValue) {
    BSONObj modObj;
    ModifierCurrentDate mod;

    // String is an invalid $currentDate argument
    modObj = fromjson("{ $currentDate : { a : 'Oct 11, 2001' } }");
    ASSERT_NOT_OK(mod.init(modObj["$currentDate"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal()));

    // Array is an invalid $currentDate argument
    modObj = fromjson("{ $currentDate : { a : [] } }");
    ASSERT_NOT_OK(mod.init(modObj["$currentDate"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal()));

    // Number is an invalid $currentDate argument
    modObj = fromjson("{ $currentDate : { a : 1 } }");
    ASSERT_NOT_OK(mod.init(modObj["$currentDate"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal()));

    // Regex is an invalid $currentDate argument
    modObj = fromjson("{ $currentDate : { a : /1/ } }");
    ASSERT_NOT_OK(mod.init(modObj["$currentDate"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal()));

    // An object with missing $type field is an invalid $currentDate argument
    modObj = fromjson("{ $currentDate : { a : { foo : 4 } } }");
    ASSERT_NOT_OK(mod.init(modObj["$currentDate"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal()));

    // An object with extra fields, including the $type field is bad
    modObj = fromjson("{ $currentDate : { a : { $type: 'date', foo : 4 } } }");
    ASSERT_NOT_OK(mod.init(modObj["$currentDate"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal()));

    // An object with extra fields, including the $type field is bad
    modObj = fromjson("{ $currentDate : { a : { foo: 4, $type : 'date' } } }");
    ASSERT_NOT_OK(mod.init(modObj["$currentDate"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal()));

    // An object with non-date/timestamp $type field is an invalid $currentDate argument
    modObj = fromjson("{ $currentDate : { a : { $type : 4 } } }");
    ASSERT_NOT_OK(mod.init(modObj["$currentDate"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal()));

    // An object with non-date/timestamp $type field is an invalid $currentDate argument
    modObj = fromjson("{ $currentDate : { a : { $type : 'foo' } } }");
    ASSERT_NOT_OK(mod.init(modObj["$currentDate"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal()));
}

TEST(BoolInput, EmptyStartDoc) {
    Document doc(fromjson("{ }"));
    Mod mod(fromjson("{ $currentDate : { a : true } }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);
    ASSERT_EQUALS("a", execInfo.fieldRef[0]->dottedField());

    BSONObj olderDateObj = fromjson("{ a : { $date : 0 } }");
    ASSERT_OK(mod.apply());
    ASSERT_LESS_THAN(olderDateObj, doc.getObject());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(mod.log(&logBuilder));
    BSONObj oplogFormat = fromjson("{ $set : { a : { $date : 0 } } }");
    validateOplogEntry(oplogFormat, logDoc);
}

TEST(DateInput, EmptyStartDoc) {
    Document doc(fromjson("{ }"));
    Mod mod(fromjson("{ $currentDate : { a : {$type: 'date' } } }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);
    ASSERT_EQUALS("a", execInfo.fieldRef[0]->dottedField());

    BSONObj olderDateObj = fromjson("{ a : { $date : 0 } }");
    ASSERT_OK(mod.apply());
    ASSERT_LESS_THAN(olderDateObj, doc.getObject());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(mod.log(&logBuilder));
    BSONObj oplogFormat = fromjson("{ $set : { a : { $date : 0 } } }");
    validateOplogEntry(oplogFormat, logDoc);
}

TEST(TimestampInput, EmptyStartDoc) {
    Document doc(fromjson("{ }"));
    Mod mod(fromjson("{ $currentDate : { a : {$type : 'timestamp' } } }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);
    ASSERT_EQUALS("a", execInfo.fieldRef[0]->dottedField());

    mongo::Timestamp ts;
    BSONObj olderDateObj = BSON("a" << ts);
    ASSERT_OK(mod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_LESS_THAN(olderDateObj, doc.getObject());

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(mod.log(&logBuilder));
    BSONObj oplogFormat = fromjson("{ $set : { a : { $timestamp : {t:0, i:0} } } }");
    validateOplogEntry(oplogFormat, logDoc);
}

TEST(BoolInput, ExistingStringDoc) {
    Document doc(fromjson("{ a: 'a' }"));
    Mod mod(fromjson("{ $currentDate : { a : true } }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);
    ASSERT_EQUALS("a", execInfo.fieldRef[0]->dottedField());

    BSONObj olderDateObj = fromjson("{ a : { $date : 0 } }");
    ASSERT_OK(mod.apply());
    ASSERT_LESS_THAN(olderDateObj, doc.getObject());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(mod.log(&logBuilder));
    BSONObj oplogFormat = fromjson("{ $set : { a : { $date : 0 } } }");
    validateOplogEntry(oplogFormat, logDoc);
}

TEST(BoolInput, ExistingDateDoc) {
    Document doc(fromjson("{ a: {$date: 0 } }"));
    Mod mod(fromjson("{ $currentDate : { a : true } }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);
    ASSERT_EQUALS("a", execInfo.fieldRef[0]->dottedField());

    BSONObj olderDateObj = fromjson("{ a : { $date : 0 } }");
    ASSERT_OK(mod.apply());
    ASSERT_LESS_THAN(olderDateObj, doc.getObject());
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(mod.log(&logBuilder));
    BSONObj oplogFormat = fromjson("{ $set : { a : { $date : 0 } } }");
    validateOplogEntry(oplogFormat, logDoc);
}

TEST(DateInput, ExistingDateDoc) {
    Document doc(fromjson("{ a: {$date: 0 } }"));
    Mod mod(fromjson("{ $currentDate : { a : {$type: 'date' } } }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);
    ASSERT_EQUALS("a", execInfo.fieldRef[0]->dottedField());

    BSONObj olderDateObj = fromjson("{ a : { $date : 0 } }");
    ASSERT_OK(mod.apply());
    ASSERT_LESS_THAN(olderDateObj, doc.getObject());
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(mod.log(&logBuilder));
    BSONObj oplogFormat = fromjson("{ $set : { a : { $date : 0 } } }");
    validateOplogEntry(oplogFormat, logDoc);
}

TEST(TimestampInput, ExistingDateDoc) {
    Document doc(fromjson("{ a: {$date: 0 } }"));
    Mod mod(fromjson("{ $currentDate : { a : {$type : 'timestamp' } } }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);
    ASSERT_EQUALS("a", execInfo.fieldRef[0]->dottedField());

    mongo::Timestamp ts;
    BSONObj olderDateObj = BSON("a" << ts);
    ASSERT_OK(mod.apply());
    ASSERT_TRUE(doc.isInPlaceModeEnabled());  // Same Size as Date
    ASSERT_LESS_THAN(olderDateObj, doc.getObject());

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(mod.log(&logBuilder));
    BSONObj oplogFormat = fromjson("{ $set : { a : { $timestamp : {t:0, i:0} } } }");
    validateOplogEntry(oplogFormat, logDoc);
}

TEST(TimestampInput, ExistingEmbeddedDateDoc) {
    Document doc(fromjson("{ a: {b: {$date: 0 } } }"));
    Mod mod(fromjson("{ $currentDate : { 'a.b' : {$type : 'timestamp' } } }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);
    ASSERT_EQUALS("a.b", execInfo.fieldRef[0]->dottedField());

    mongo::Timestamp ts;
    BSONObj olderDateObj = BSON("a" << BSON("b" << ts));
    ASSERT_OK(mod.apply());
    ASSERT_TRUE(doc.isInPlaceModeEnabled());  // Same Size as Date
    ASSERT_LESS_THAN(olderDateObj, doc.getObject());

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(mod.log(&logBuilder));
    BSONObj oplogFormat = fromjson("{ $set : { 'a.b' : { $timestamp : {t:0, i:0} } } }");
    validateOplogEntry(oplogFormat, logDoc);
}

TEST(DottedTimestampInput, EmptyStartDoc) {
    Document doc(fromjson("{ }"));
    Mod mod(fromjson("{ $currentDate : { 'a.b' : {$type : 'timestamp' } } }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);
    ASSERT_EQUALS("a.b", execInfo.fieldRef[0]->dottedField());

    mongo::Timestamp ts;
    BSONObj olderDateObj = BSON("a" << BSON("b" << ts));
    ASSERT_OK(mod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_LESS_THAN(olderDateObj, doc.getObject());

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(mod.log(&logBuilder));
    BSONObj oplogFormat = fromjson("{ $set : { 'a.b' : { $timestamp : {t:0, i:0} } } }");
    validateOplogEntry(oplogFormat, logDoc);
}

}  // namespace
