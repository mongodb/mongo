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


#include "mongo/db/ops/modifier_inc.h"

#include <cstdint>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/mutable_bson_test_utils.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/ops/log_builder.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/unittest.h"

namespace {

using mongo::BSONObj;
using mongo::Decimal128;
using mongo::LogBuilder;
using mongo::ModifierInc;
using mongo::ModifierInterface;
using mongo::NumberInt;
using mongo::Status;
using mongo::StringData;
using mongo::fromjson;
using mongo::mutablebson::ConstElement;
using mongo::mutablebson::Document;
using mongo::mutablebson::Element;
using mongo::mutablebson::countChildren;

/** Helper to build and manipulate a $inc/$mul modifier */
class Mod {
public:
    explicit Mod(BSONObj modObj)
        : _modObj(modObj),
          _mod(mongoutils::str::equals(modObj.firstElement().fieldName(), "$mul")
                   ? ModifierInc::MODE_MUL
                   : ModifierInc::MODE_INC) {
        StringData modName = modObj.firstElement().fieldName();
        ASSERT_OK(_mod.init(_modObj[modName].embeddedObject().firstElement(),
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

    ModifierInc& mod() {
        return _mod;
    }

private:
    BSONObj _modObj;
    ModifierInc _mod;
};

TEST(Init, FailToInitWithInvalidValue) {
    BSONObj modObj;
    ModifierInc mod;

    // String is an invalid increment argument
    modObj = fromjson("{ $inc : { a : '' } }");
    ASSERT_NOT_OK(mod.init(modObj["$inc"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal()));

    // Object is an invalid increment argument
    modObj = fromjson("{ $inc : { a : {} } }");
    ASSERT_NOT_OK(mod.init(modObj["$inc"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal()));

    // Array is an invalid increment argument
    modObj = fromjson("{ $inc : { a : [] } }");
    ASSERT_NOT_OK(mod.init(modObj["$inc"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal()));
}

TEST(Init, InitParsesNumberInt) {
    Mod incMod(BSON("$inc" << BSON("a" << static_cast<int>(1))));
}

TEST(Init, InitParsesNumberLong) {
    Mod incMod(BSON("$inc" << BSON("a" << static_cast<long long>(1))));
}

TEST(Init, InitParsesNumberDouble) {
    Mod incMod(BSON("$inc" << BSON("a" << 1.0)));
}

TEST(Init, InitParsesNumberDecimal) {
    Mod incMod(BSON("$inc" << BSON("a" << Decimal128(1.0))));
}

TEST(SimpleMod, PrepareSimpleOK) {
    Document doc(fromjson("{ a : 1 }"));
    Mod incMod(fromjson("{ $inc: { a : 1 }}"));

    ModifierInterface::ExecInfo execInfo;

    ASSERT_OK(incMod.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_FALSE(execInfo.noOp);
}

TEST(SimpleMod, PrepareSimpleNonNumericObject) {
    Document doc(fromjson("{ a : {} }"));
    Mod incMod(fromjson("{ $inc: { a : 1 }}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_NOT_OK(incMod.prepare(doc.root(), "", &execInfo));
}

TEST(SimpleMod, PrepareSimpleNonNumericArray) {
    Document doc(fromjson("{ a : [] }"));
    Mod incMod(fromjson("{ $inc: { a : 1 }}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_NOT_OK(incMod.prepare(doc.root(), "", &execInfo));
}

TEST(SimpleMod, PrepareSimpleNonNumericString) {
    Document doc(fromjson("{ a : '' }"));
    Mod incMod(fromjson("{ $inc: { a : 1 }}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_NOT_OK(incMod.prepare(doc.root(), "", &execInfo));
}

TEST(SimpleMod, ApplyAndLogEmptyDocument) {
    Document doc(fromjson("{}"));
    Mod incMod(fromjson("{ $inc: { a : 1 }}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(incMod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(incMod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{ a : 1 }"), doc);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(incMod.log(&logBuilder));
    ASSERT_EQUALS(fromjson("{ $set : { a : 1 } }"), logDoc);
}

TEST(SimpleMod, LogWithoutApplyEmptyDocument) {
    Document doc(fromjson("{}"));
    Mod incMod(fromjson("{ $inc: { a : 1 }}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(incMod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(incMod.log(&logBuilder));
    ASSERT_EQUALS(fromjson("{ $set : { a : 1 } }"), logDoc);
}

TEST(SimpleMod, ApplyAndLogSimpleDocument) {
    Document doc(fromjson("{ a : 2 }"));
    Mod incMod(fromjson("{ $inc: { a : 1 }}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(incMod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(incMod.apply());
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{ a : 3 }"), doc);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(incMod.log(&logBuilder));
    ASSERT_EQUALS(fromjson("{ $set : { a : 3 } }"), logDoc);
}

TEST(DottedMod, ApplyAndLogSimpleDocument) {
    Document doc(fromjson("{ a : { b : 2 } }"));
    Mod incMod(fromjson("{ $inc: { 'a.b' : 1 } }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(incMod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(incMod.apply());
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{ a : { b : 3 } }"), doc);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(incMod.log(&logBuilder));
    ASSERT_EQUALS(fromjson("{ $set : { 'a.b' : 3 } }"), logDoc);
}

TEST(InPlace, IntToInt) {
    Document doc(BSON("a" << static_cast<int>(1)));
    Mod incMod(BSON("$inc" << BSON("a" << static_cast<int>(1))));
    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(incMod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);
}

TEST(InPlace, LongToLong) {
    Document doc(BSON("a" << static_cast<long long>(1)));
    Mod incMod(BSON("$inc" << BSON("a" << static_cast<long long>(1))));
    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(incMod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);
}

TEST(InPlace, DoubleToDouble) {
    Document doc(BSON("a" << 1.0));
    Mod incMod(BSON("$inc" << BSON("a" << 1.0)));
    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(incMod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);
}

TEST(NoOp, Int) {
    Document doc(BSON("a" << static_cast<int>(1)));
    Mod incMod(BSON("$inc" << BSON("a" << static_cast<int>(0))));
    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(incMod.prepare(doc.root(), "", &execInfo));
    ASSERT_TRUE(execInfo.noOp);
}

TEST(NoOp, Long) {
    Document doc(BSON("a" << static_cast<long long>(1)));
    Mod incMod(BSON("$inc" << BSON("a" << static_cast<long long>(0))));
    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(incMod.prepare(doc.root(), "", &execInfo));
    ASSERT_TRUE(execInfo.noOp);
}

TEST(NoOp, Double) {
    Document doc(BSON("a" << 1.0));
    Mod incMod(BSON("$inc" << BSON("a" << 0.0)));
    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(incMod.prepare(doc.root(), "", &execInfo));
    ASSERT_TRUE(execInfo.noOp);
}

TEST(NoOp, Decimal) {
    Document doc(BSON("a" << Decimal128("1.0")));
    Mod incMod(BSON("$inc" << BSON("a" << Decimal128("0.0"))));
    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(incMod.prepare(doc.root(), "", &execInfo));
    ASSERT_TRUE(execInfo.noOp);
}

TEST(Upcasting, UpcastIntToLong) {
    // Checks that $inc : NumberLong(0) turns a NumberInt into a NumberLong and logs it
    // correctly.
    Document doc(BSON("a" << static_cast<int>(1)));
    ASSERT_EQUALS(mongo::NumberInt, doc.root()["a"].getType());

    Mod incMod(BSON("$inc" << BSON("a" << static_cast<long long>(0))));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(incMod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(incMod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{ a : 1 }"), doc);
    ASSERT_EQUALS(mongo::NumberLong, doc.root()["a"].getType());

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(incMod.log(&logBuilder));
    ASSERT_EQUALS(fromjson("{ $set : { a : 1 } }"), logDoc);
    ASSERT_EQUALS(mongo::NumberLong, logDoc.root()["$set"]["a"].getType());
}

TEST(Upcasting, UpcastIntToDouble) {
    // Checks that $inc : 0.0 turns a NumberInt into a NumberDouble and logs it
    // correctly.
    Document doc(BSON("a" << static_cast<int>(1)));
    ASSERT_EQUALS(mongo::NumberInt, doc.root()["a"].getType());

    Mod incMod(fromjson("{ $inc : { a : 0.0 } }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(incMod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(incMod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{ a : 1.0 }"), doc);
    ASSERT_EQUALS(mongo::NumberDouble, doc.root()["a"].getType());

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(incMod.log(&logBuilder));
    ASSERT_EQUALS(fromjson("{ $set : { a : 1.0 } }"), logDoc);
    ASSERT_EQUALS(mongo::NumberDouble, logDoc.root()["$set"]["a"].getType());
}

TEST(Upcasting, UpcastLongToDouble) {
    // Checks that $inc : 0.0 turns a NumberLong into a NumberDouble and logs it
    // correctly.
    Document doc(BSON("a" << static_cast<long long>(1)));
    ASSERT_EQUALS(mongo::NumberLong, doc.root()["a"].getType());

    Mod incMod(fromjson("{ $inc : { a : 0.0 } }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(incMod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(incMod.apply());
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{ a : 1.0 }"), doc);
    ASSERT_EQUALS(mongo::NumberDouble, doc.root()["a"].getType());

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(incMod.log(&logBuilder));
    ASSERT_EQUALS(fromjson("{ $set : { a : 1.0 } }"), logDoc);
    ASSERT_EQUALS(mongo::NumberDouble, logDoc.root()["$set"]["a"].getType());
}

TEST(Upcasting, DoublesStayDoubles) {
    // Checks that $inc : 0 doesn't change a NumberDouble away from double
    Document doc(fromjson("{ a : 1.0 }"));
    ASSERT_EQUALS(mongo::NumberDouble, doc.root()["a"].getType());

    Mod incMod(fromjson("{ $inc : { a : 1 } }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(incMod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(incMod.apply());
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{ a : 2.0 }"), doc);
    ASSERT_EQUALS(mongo::NumberDouble, doc.root()["a"].getType());

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(incMod.log(&logBuilder));
    ASSERT_EQUALS(fromjson("{ $set : { a : 2.0 } }"), logDoc);
    ASSERT_EQUALS(mongo::NumberDouble, logDoc.root()["$set"]["a"].getType());
}

TEST(Upcasting, UpcastIntToDecimal) {
    // Checks that $inc : NumberDecimal(0) turns a NumberInt into a NumberDecimal and logs it
    // correctly.
    Document doc(BSON("a" << static_cast<int>(1)));
    ASSERT_EQUALS(mongo::NumberInt, doc.root()["a"].getType());

    Mod incMod(fromjson("{ $inc : { a : NumberDecimal(\"0\") }}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(incMod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(incMod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{ a : NumberDecimal(\"1.0\") }"), doc);
    ASSERT_EQUALS(mongo::NumberDecimal, doc.root()["a"].getType());

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(incMod.log(&logBuilder));
    ASSERT_EQUALS(fromjson("{ $set : { a : NumberDecimal(\"1.0\") }}"), logDoc);
    ASSERT_EQUALS(mongo::NumberDecimal, logDoc.root()["$set"]["a"].getType());
}

TEST(Upcasting, UpcastLongToDecimal) {
    // Checks that $inc : NumberDecimal(0) turns a NumberLong into a NumberDecimal and logs it
    // correctly.
    Document doc(BSON("a" << static_cast<long long>(1)));
    ASSERT_EQUALS(mongo::NumberLong, doc.root()["a"].getType());

    Mod incMod(fromjson("{ $inc : { a : NumberDecimal(\"0\") }}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(incMod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(incMod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{ a : NumberDecimal(\"1.0\") }"), doc);
    ASSERT_EQUALS(mongo::NumberDecimal, doc.root()["a"].getType());

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(incMod.log(&logBuilder));
    ASSERT_EQUALS(fromjson("{ $set : { a : NumberDecimal(\"1.0\") }}"), logDoc);
    ASSERT_EQUALS(mongo::NumberDecimal, logDoc.root()["$set"]["a"].getType());
}

TEST(Upcasting, UpcastDoubleToDecimal) {
    // Checks that $inc : NumberDecimal(0) turns a double into a NumberDecimal and logs it
    // correctly.
    Document doc(BSON("a" << static_cast<double>(1.0)));
    ASSERT_EQUALS(mongo::NumberDouble, doc.root()["a"].getType());

    Mod incMod(fromjson("{ $inc : { a : NumberDecimal(\"0\") }}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(incMod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(incMod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{ a : NumberDecimal(\"1.0\") }"), doc);
    ASSERT_EQUALS(mongo::NumberDecimal, doc.root()["a"].getType());

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(incMod.log(&logBuilder));
    ASSERT_EQUALS(fromjson("{ $set : { a : NumberDecimal(\"1.0\") }}"), logDoc);
    ASSERT_EQUALS(mongo::NumberDecimal, logDoc.root()["$set"]["a"].getType());
}

TEST(Upcasting, DecimalsStayDecimals) {
    // Checks that $inc : NumberDecimal(1) keeps a NumberDecimal as a NumberDecimal and logs it
    // correctly.
    Document doc(BSON("a" << mongo::Decimal128("1.0")));
    ASSERT_EQUALS(mongo::NumberDecimal, doc.root()["a"].getType());

    Mod incMod(fromjson("{ $inc : { a : NumberDecimal(\"1\") }}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(incMod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(incMod.apply());
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{ a : NumberDecimal(\"2.0\") }"), doc);
    ASSERT_EQUALS(mongo::NumberDecimal, doc.root()["a"].getType());

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(incMod.log(&logBuilder));
    ASSERT_EQUALS(fromjson("{ $set : { a : NumberDecimal(\"2.0\") }}"), logDoc);
    ASSERT_EQUALS(mongo::NumberDecimal, logDoc.root()["$set"]["a"].getType());
}

// The only interesting overflow cases are int->long via increment: we never overflow to
// double, and we never decrease precision on decrement.
TEST(Spilling, OverflowIntToLong) {
    const int initial_value = std::numeric_limits<int32_t>::max();

    Document doc(BSON("a" << static_cast<int>(initial_value)));
    ASSERT_EQUALS(mongo::NumberInt, doc.root()["a"].getType());

    Mod incMod(fromjson("{ $inc : { a : 1 } }"));
    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(incMod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);

    const long long target_value = static_cast<long long>(initial_value) + 1;

    ASSERT_OK(incMod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(BSON("a" << target_value), doc);
}

TEST(Spilling, UnderflowIntToLong) {
    const int initial_value = std::numeric_limits<int32_t>::min();

    Document doc(BSON("a" << static_cast<int>(initial_value)));
    ASSERT_EQUALS(mongo::NumberInt, doc.root()["a"].getType());

    Mod incMod(fromjson("{ $inc : { a : -1 } }"));
    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(incMod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);

    const long long target_value = static_cast<long long>(initial_value) - 1;

    ASSERT_OK(incMod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(BSON("a" << target_value), doc);
}

TEST(Lifecycle, IncModCanBeReused) {
    Document doc1(fromjson("{ a : 1 }"));
    Document doc2(fromjson("{ a : 1 }"));

    Mod incMod(fromjson("{ $inc: { a : 1 }}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(incMod.prepare(doc1.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(incMod.apply());
    ASSERT_TRUE(doc1.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{ a : 2 }"), doc1);

    ASSERT_OK(incMod.prepare(doc2.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(incMod.apply());
    ASSERT_TRUE(doc2.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{ a : 2 }"), doc2);
}

// Given the current implementation of $mul, we really only need one test for
// $mul. However, in the future, we should probably write additional ones, or, perhaps find
// a way to run all the above tests in both modes.
TEST(Multiplication, ApplyAndLogSimpleDocument) {
    Document doc(fromjson("{ a : { b : 2 } }"));
    Mod incMod(fromjson("{ $mul: { 'a.b' : 3 } }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(incMod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(incMod.apply());
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{ a : { b : 6 } }"), doc);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(incMod.log(&logBuilder));
    ASSERT_EQUALS(fromjson("{ $set : { 'a.b' : 6 } }"), logDoc);
}

TEST(Multiplication, ApplyAndLogMissingElement) {
    Document doc(fromjson("{ a : 0 }"));
    Mod incMod(fromjson("{ $mul : { b : 3 } }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(incMod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(incMod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{ a : 0, b : 0 }"), doc);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(incMod.log(&logBuilder));
    ASSERT_EQUALS(fromjson("{ $set : { b : 0 } }"), logDoc);
}

TEST(Multiplication, ApplyMissingElementInt) {
    const int int_zero = 0;
    const int int_three = 3;

    Document doc(BSON("a" << int_zero));
    Mod incMod(BSON("$mul" << BSON("b" << int_three)));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(incMod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(incMod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(BSON("a" << int_zero << "b" << int_zero), doc);
    ASSERT_EQUALS(mongo::NumberInt, doc.root().rightChild().getType());
}

TEST(Multiplication, ApplyMissingElementLongLong) {
    const long long ll_zero = 0;
    const long long ll_three = 3;

    Document doc(BSON("a" << ll_zero));
    Mod incMod(BSON("$mul" << BSON("b" << ll_three)));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(incMod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(incMod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(BSON("a" << ll_zero << "b" << ll_zero), doc);
    ASSERT_EQUALS(mongo::NumberLong, doc.root().rightChild().getType());
}

TEST(Multiplication, ApplyMissingElementDouble) {
    const double double_zero = 0;
    const double double_three = 3;

    Document doc(BSON("a" << double_zero));
    Mod incMod(BSON("$mul" << BSON("b" << double_three)));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(incMod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(incMod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(BSON("a" << double_zero << "b" << 0), doc);
    ASSERT_EQUALS(mongo::NumberDouble, doc.root().rightChild().getType());
}

}  // namespace
