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

#include "mongo/db/ops/modifier_compare.h"

#include <cstdint>

#include "mongo/base/string_data.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/mutable_bson_test_utils.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/ops/log_builder.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/unittest.h"

namespace {

using mongo::BSONObj;
using mongo::CollatorInterfaceMock;
using mongo::LogBuilder;
using mongo::ModifierCompare;
using mongo::ModifierInterface;
using mongo::Status;
using mongo::StringData;
using mongo::fromjson;
using mongo::mutablebson::ConstElement;
using mongo::mutablebson::Document;
using mongo::mutablebson::Element;

const char kModNameMin[] = "$min";
const char kModNameMax[] = "$max";

/** Helper to build and manipulate a $min/max mod. */
class Mod {
public:
    Mod() : _mod() {}

    explicit Mod(BSONObj modObj,
                 ModifierInterface::Options options = ModifierInterface::Options::normal())
        : _modObj(modObj),
          _mod((modObj.firstElement().fieldNameStringData() == "$min") ? ModifierCompare::MIN
                                                                       : ModifierCompare::MAX) {
        StringData modName = modObj.firstElement().fieldName();
        ASSERT_OK(_mod.init(modObj[modName].embeddedObject().firstElement(), options));
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

    ModifierCompare& mod() {
        return _mod;
    }

private:
    BSONObj _modObj;
    ModifierCompare _mod;
};

TEST(Init, ValidValues) {
    BSONObj modObj;
    ModifierCompare mod;

    modObj = fromjson("{ $min : { a : 2 } }");
    ASSERT_OK(mod.init(modObj[kModNameMin].embeddedObject().firstElement(),
                       ModifierInterface::Options::normal()));

    modObj = fromjson("{ $max : { a : 1 } }");
    ASSERT_OK(mod.init(modObj[kModNameMax].embeddedObject().firstElement(),
                       ModifierInterface::Options::normal()));

    modObj = fromjson("{ $min : { a : {$date : 0 } } }");
    ASSERT_OK(mod.init(modObj[kModNameMin].embeddedObject().firstElement(),
                       ModifierInterface::Options::normal()));
}

TEST(ExistingNumber, MaxSameNumber) {
    Document doc(fromjson("{a: 1 }"));
    Mod mod(fromjson("{$max: {a: 1} }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_TRUE(execInfo.noOp);
}

TEST(ExistingNumber, MinSameNumber) {
    Document doc(fromjson("{a: 1 }"));
    Mod mod(fromjson("{$min: {a: 1} }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_TRUE(execInfo.noOp);
}

TEST(ExistingNumber, MaxNumberIsLess) {
    Document doc(fromjson("{a: 1 }"));
    Mod mod(fromjson("{$max: {a: 0} }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_TRUE(execInfo.noOp);
}

TEST(ExistingNumber, MinNumberIsMore) {
    Document doc(fromjson("{a: 1 }"));
    Mod mod(fromjson("{$min: {a: 2} }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_TRUE(execInfo.noOp);
}

TEST(ExistingDouble, MaxSameValInt) {
    Document doc(fromjson("{a: 1.0 }"));
    Mod mod(BSON("$max" << BSON("a" << 1LL)));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_TRUE(execInfo.noOp);
}

TEST(ExistingDoubleZero, MaxSameValIntZero) {
    Document doc(fromjson("{a: 0.0 }"));
    Mod mod(BSON("$max" << BSON("a" << 0LL)));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_TRUE(execInfo.noOp);
}

TEST(ExistingDoubleZero, MinSameValIntZero) {
    Document doc(fromjson("{a: 0.0 }"));
    Mod mod(BSON("$min" << BSON("a" << 0LL)));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_TRUE(execInfo.noOp);
}

TEST(MissingField, MinNumber) {
    Document doc(fromjson("{}"));
    Mod mod(fromjson("{$min: {a: 0} }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);
    ASSERT_EQUALS("a", execInfo.fieldRef[0]->dottedField());

    ASSERT_OK(mod.apply());
    ASSERT_EQUALS(fromjson("{a : 0}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(mod.log(&logBuilder));
    ASSERT_EQUALS(fromjson("{ $set : { a : 0 } }"), logDoc);
}

TEST(ExistingNumber, MinNumber) {
    Document doc(fromjson("{a: 1 }"));
    Mod mod(fromjson("{$min: {a: 0} }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);
    ASSERT_EQUALS("a", execInfo.fieldRef[0]->dottedField());

    ASSERT_OK(mod.apply());
    ASSERT_EQUALS(fromjson("{a : 0}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(mod.log(&logBuilder));
    ASSERT_EQUALS(fromjson("{ $set : { a : 0 } }"), logDoc);
}

TEST(MissingField, MaxNumber) {
    Document doc(fromjson("{}"));
    Mod mod(fromjson("{$max: {a: 0} }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);
    ASSERT_EQUALS("a", execInfo.fieldRef[0]->dottedField());

    ASSERT_OK(mod.apply());
    ASSERT_EQUALS(fromjson("{a : 0}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(mod.log(&logBuilder));
    ASSERT_EQUALS(fromjson("{ $set : { a : 0 } }"), logDoc);
}

TEST(ExistingNumber, MaxNumber) {
    Document doc(fromjson("{a: 1 }"));
    Mod mod(fromjson("{$max: {a: 2} }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);
    ASSERT_EQUALS("a", execInfo.fieldRef[0]->dottedField());

    ASSERT_OK(mod.apply());
    ASSERT_EQUALS(fromjson("{a : 2}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(mod.log(&logBuilder));
    ASSERT_EQUALS(fromjson("{ $set : { a : 2 } }"), logDoc);
}

TEST(ExistingDate, MaxDate) {
    Document doc(fromjson("{a: {$date: 0} }"));
    Mod mod(fromjson("{$max: {a: {$date: 123123123}} }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);
    ASSERT_EQUALS("a", execInfo.fieldRef[0]->dottedField());

    ASSERT_OK(mod.apply());
    ASSERT_EQUALS(fromjson("{a: {$date: 123123123}}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(mod.log(&logBuilder));
    ASSERT_EQUALS(fromjson("{$set: {a: {$date: 123123123}} }"), logDoc);
}

TEST(ExistingEmbeddedDoc, MaxDoc) {
    Document doc(fromjson("{a: {b: 2}}"));
    Mod mod(fromjson("{$max: {a: {b: 3}}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);
    ASSERT_EQUALS("a", execInfo.fieldRef[0]->dottedField());

    ASSERT_OK(mod.apply());
    ASSERT_EQUALS(fromjson("{a: {b: 3}}}"), doc);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(mod.log(&logBuilder));
    ASSERT_EQUALS(fromjson("{$set: {a: {b: 3}} }"), logDoc);
}

TEST(ExistingEmbeddedDoc, MaxNumber) {
    Document doc(fromjson("{a: {b: 2}}"));
    Mod mod(fromjson("{$max: {a: 3}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_TRUE(execInfo.noOp);
    ASSERT_EQUALS("a", execInfo.fieldRef[0]->dottedField());
}

TEST(Collation, MinRespectsCollationFromModifierInterfaceOptions) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    Document doc(fromjson("{a: 'cbc'}"));
    ModifierInterface::Options options = ModifierInterface::Options::normal(&collator);
    Mod mod(fromjson("{$min: {a: 'dba'}}"), options);

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_EQUALS("a", execInfo.fieldRef[0]->dottedField());

    ASSERT_OK(mod.apply());
    ASSERT_EQUALS(fromjson("{a : 'dba'}"), doc);
}

TEST(Collation, MinRespectsCollationFromSetCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    Document doc(fromjson("{a: 'cbc'}"));
    Mod mod(fromjson("{$min: {a: 'dba'}}"));
    mod.mod().setCollator(&collator);

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_EQUALS("a", execInfo.fieldRef[0]->dottedField());

    ASSERT_OK(mod.apply());
    ASSERT_EQUALS(fromjson("{a : 'dba'}"), doc);
}

TEST(Collation, MaxRespectsCollationFromSetCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    Document doc(fromjson("{a: 'cbc'}"));
    Mod mod(fromjson("{$max: {a: 'abd'}}"));
    mod.mod().setCollator(&collator);

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_EQUALS("a", execInfo.fieldRef[0]->dottedField());

    ASSERT_OK(mod.apply());
    ASSERT_EQUALS(fromjson("{a : 'abd'}"), doc);
}
}  // namespace
