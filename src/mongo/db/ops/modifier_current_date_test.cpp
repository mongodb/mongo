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
#include "mongo/db/logical_clock.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/db/update/log_builder.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"

namespace {

using mongo::BSONObj;
using mongo::ExpressionContextForTest;
using mongo::LogBuilder;
using mongo::ModifierCurrentDate;
using mongo::ModifierInterface;
using mongo::Status;
using mongo::StringData;
using mongo::Timestamp;
using mongo::fromjson;
using mongo::mutablebson::ConstElement;
using mongo::mutablebson::Document;
using mongo::mutablebson::Element;

class ModifierCurrentDateTest : public mongo::unittest::Test {
public:
    ~ModifierCurrentDateTest() override = default;

protected:
    /**
     * Sets up this fixture with a context and a LogicalClock.
     */
    void setUp() override {
        auto service = mongo::getGlobalServiceContext();

        auto logicalClock = mongo::stdx::make_unique<mongo::LogicalClock>(service);
        mongo::LogicalClock::set(service, std::move(logicalClock));
    }
    void tearDown() override{};
};

using Init = ModifierCurrentDateTest;
using BoolInput = ModifierCurrentDateTest;
using DateInput = ModifierCurrentDateTest;
using TimestampInput = ModifierCurrentDateTest;
using DottedTimestampInput = ModifierCurrentDateTest;

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
                            ModifierInterface::Options::normal(new ExpressionContextForTest())));
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

TEST_F(Init, ValidValues) {
    BSONObj modObj;
    ModifierCurrentDate mod;
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());

    modObj = fromjson("{ $currentDate : { a : true } }");
    ASSERT_OK(mod.init(modObj["$currentDate"].embeddedObject().firstElement(),
                       ModifierInterface::Options::normal(expCtx)));

    modObj = fromjson("{ $currentDate : { a : {$type : 'timestamp' } } }");
    ASSERT_OK(mod.init(modObj["$currentDate"].embeddedObject().firstElement(),
                       ModifierInterface::Options::normal(expCtx)));

    modObj = fromjson("{ $currentDate : { a : {$type : 'date' } } }");
    ASSERT_OK(mod.init(modObj["$currentDate"].embeddedObject().firstElement(),
                       ModifierInterface::Options::normal(expCtx)));
}

TEST_F(Init, FailToInitWithInvalidValue) {
    BSONObj modObj;
    ModifierCurrentDate mod;
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());

    // String is an invalid $currentDate argument
    modObj = fromjson("{ $currentDate : { a : 'Oct 11, 2001' } }");
    ASSERT_NOT_OK(mod.init(modObj["$currentDate"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal(expCtx)));

    // Array is an invalid $currentDate argument
    modObj = fromjson("{ $currentDate : { a : [] } }");
    ASSERT_NOT_OK(mod.init(modObj["$currentDate"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal(expCtx)));

    // Number is an invalid $currentDate argument
    modObj = fromjson("{ $currentDate : { a : 1 } }");
    ASSERT_NOT_OK(mod.init(modObj["$currentDate"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal(expCtx)));

    // Regex is an invalid $currentDate argument
    modObj = fromjson("{ $currentDate : { a : /1/ } }");
    ASSERT_NOT_OK(mod.init(modObj["$currentDate"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal(expCtx)));

    // An object with missing $type field is an invalid $currentDate argument
    modObj = fromjson("{ $currentDate : { a : { foo : 4 } } }");
    ASSERT_NOT_OK(mod.init(modObj["$currentDate"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal(expCtx)));

    // An object with extra fields, including the $type field is bad
    modObj = fromjson("{ $currentDate : { a : { $type: 'date', foo : 4 } } }");
    ASSERT_NOT_OK(mod.init(modObj["$currentDate"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal(expCtx)));

    // An object with extra fields, including the $type field is bad
    modObj = fromjson("{ $currentDate : { a : { foo: 4, $type : 'date' } } }");
    ASSERT_NOT_OK(mod.init(modObj["$currentDate"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal(expCtx)));

    // An object with non-date/timestamp $type field is an invalid $currentDate argument
    modObj = fromjson("{ $currentDate : { a : { $type : 4 } } }");
    ASSERT_NOT_OK(mod.init(modObj["$currentDate"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal(expCtx)));

    // An object with non-date/timestamp $type field is an invalid $currentDate argument
    modObj = fromjson("{ $currentDate : { a : { $type : 'foo' } } }");
    ASSERT_NOT_OK(mod.init(modObj["$currentDate"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal(expCtx)));
}

TEST_F(BoolInput, EmptyStartDoc) {
    Document doc(fromjson("{ }"));
    Mod mod(fromjson("{ $currentDate : { a : true } }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);
    ASSERT_EQUALS("a", execInfo.fieldRef[0]->dottedField());

    BSONObj olderDateObj = fromjson("{ a : { $date : 0 } }");
    ASSERT_OK(mod.apply());
    ASSERT_BSONOBJ_LT(olderDateObj, doc.getObject());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(mod.log(&logBuilder));
    BSONObj oplogFormat = fromjson("{ $set : { a : { $date : 0 } } }");
    validateOplogEntry(oplogFormat, logDoc);
}

TEST_F(DateInput, EmptyStartDoc) {
    Document doc(fromjson("{ }"));
    Mod mod(fromjson("{ $currentDate : { a : {$type: 'date' } } }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);
    ASSERT_EQUALS("a", execInfo.fieldRef[0]->dottedField());

    BSONObj olderDateObj = fromjson("{ a : { $date : 0 } }");
    ASSERT_OK(mod.apply());
    ASSERT_BSONOBJ_LT(olderDateObj, doc.getObject());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(mod.log(&logBuilder));
    BSONObj oplogFormat = fromjson("{ $set : { a : { $date : 0 } } }");
    validateOplogEntry(oplogFormat, logDoc);
}

TEST_F(TimestampInput, EmptyStartDoc) {
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
    ASSERT_BSONOBJ_LT(olderDateObj, doc.getObject());

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(mod.log(&logBuilder));
    BSONObj oplogFormat = fromjson("{ $set : { a : { $timestamp : {t:0, i:0} } } }");
    validateOplogEntry(oplogFormat, logDoc);
}

TEST_F(BoolInput, ExistingStringDoc) {
    Document doc(fromjson("{ a: 'a' }"));
    Mod mod(fromjson("{ $currentDate : { a : true } }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);
    ASSERT_EQUALS("a", execInfo.fieldRef[0]->dottedField());

    BSONObj olderDateObj = fromjson("{ a : { $date : 0 } }");
    ASSERT_OK(mod.apply());
    ASSERT_BSONOBJ_LT(olderDateObj, doc.getObject());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(mod.log(&logBuilder));
    BSONObj oplogFormat = fromjson("{ $set : { a : { $date : 0 } } }");
    validateOplogEntry(oplogFormat, logDoc);
}

TEST_F(BoolInput, ExistingDateDoc) {
    Document doc(fromjson("{ a: {$date: 0 } }"));
    Mod mod(fromjson("{ $currentDate : { a : true } }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);
    ASSERT_EQUALS("a", execInfo.fieldRef[0]->dottedField());

    BSONObj olderDateObj = fromjson("{ a : { $date : 0 } }");
    ASSERT_OK(mod.apply());
    ASSERT_BSONOBJ_LT(olderDateObj, doc.getObject());
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(mod.log(&logBuilder));
    BSONObj oplogFormat = fromjson("{ $set : { a : { $date : 0 } } }");
    validateOplogEntry(oplogFormat, logDoc);
}

TEST_F(DateInput, ExistingDateDoc) {
    Document doc(fromjson("{ a: {$date: 0 } }"));
    Mod mod(fromjson("{ $currentDate : { a : {$type: 'date' } } }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);
    ASSERT_EQUALS("a", execInfo.fieldRef[0]->dottedField());

    BSONObj olderDateObj = fromjson("{ a : { $date : 0 } }");
    ASSERT_OK(mod.apply());
    ASSERT_BSONOBJ_LT(olderDateObj, doc.getObject());
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(mod.log(&logBuilder));
    BSONObj oplogFormat = fromjson("{ $set : { a : { $date : 0 } } }");
    validateOplogEntry(oplogFormat, logDoc);
}

TEST_F(TimestampInput, ExistingDateDoc) {
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
    ASSERT_BSONOBJ_LT(olderDateObj, doc.getObject());

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(mod.log(&logBuilder));
    BSONObj oplogFormat = fromjson("{ $set : { a : { $timestamp : {t:0, i:0} } } }");
    validateOplogEntry(oplogFormat, logDoc);
}

TEST_F(TimestampInput, ExistingEmbeddedDateDoc) {
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
    ASSERT_BSONOBJ_LT(olderDateObj, doc.getObject());

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(mod.log(&logBuilder));
    BSONObj oplogFormat = fromjson("{ $set : { 'a.b' : { $timestamp : {t:0, i:0} } } }");
    validateOplogEntry(oplogFormat, logDoc);
}

TEST_F(DottedTimestampInput, EmptyStartDoc) {
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
    ASSERT_BSONOBJ_LT(olderDateObj, doc.getObject());

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(mod.log(&logBuilder));
    BSONObj oplogFormat = fromjson("{ $set : { 'a.b' : { $timestamp : {t:0, i:0} } } }");
    validateOplogEntry(oplogFormat, logDoc);
}

TEST_F(BoolInput, PrepareReportCreatedArrayElement) {
    Document doc(fromjson("{a: [{b: 0}]}"));
    Mod mod(fromjson("{$currentDate: {'a.1.c': true}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.1.c");
    ASSERT_TRUE(execInfo.indexOfArrayWithNewElement[0]);
    ASSERT_EQUALS(*execInfo.indexOfArrayWithNewElement[0], 0u);
    ASSERT_FALSE(execInfo.noOp);
}

TEST_F(BoolInput, PrepareDoNotReportModifiedArrayElement) {
    Document doc(fromjson("{a: [{b: 0}]}"));
    Mod mod(fromjson("{$currentDate: {'a.0.c': true}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.0.c");
    ASSERT_FALSE(execInfo.indexOfArrayWithNewElement[0]);
    ASSERT_FALSE(execInfo.noOp);
}

TEST_F(BoolInput, PrepareDoNotReportCreatedNumericObjectField) {
    Document doc(fromjson("{a: {'0': {b: 0}}}"));
    Mod mod(fromjson("{$currentDate: {'a.1.c': true}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.1.c");
    ASSERT_FALSE(execInfo.indexOfArrayWithNewElement[0]);
    ASSERT_FALSE(execInfo.noOp);
}

}  // namespace
