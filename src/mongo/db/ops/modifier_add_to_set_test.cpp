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


#include "mongo/db/ops/modifier_add_to_set.h"

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
using mongo::ModifierAddToSet;
using mongo::ModifierInterface;
using mongo::Status;
using mongo::StringData;
using mongo::fromjson;
using mongo::mutablebson::Document;
using mongo::mutablebson::Element;

/** Helper to build and manipulate a $addToSet mod. */
class Mod {
public:
    Mod() : _mod() {}

    explicit Mod(BSONObj modObj,
                 ModifierInterface::Options options = ModifierInterface::Options::normal())
        : _modObj(modObj), _mod() {
        ASSERT_OK(_mod.init(_modObj["$addToSet"].embeddedObject().firstElement(), options));
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

    ModifierAddToSet& mod() {
        return _mod;
    }

private:
    BSONObj _modObj;
    ModifierAddToSet _mod;
};

TEST(Init, FailToInitWithInvalidValue) {
    BSONObj modObj;
    ModifierAddToSet mod;

    modObj = fromjson("{ $addToSet : { a : { 'x.$.y' : 'bad' } } }");
    ASSERT_NOT_OK(mod.init(modObj["$addToSet"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal()));
    modObj = fromjson("{ $addToSet : { a : { $each : [ { 'x.$.y' : 'bad' } ] } } }");
    ASSERT_NOT_OK(mod.init(modObj["$addToSet"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal()));

    // An int is not valid after $each
    modObj = fromjson("{ $addToSet : { a : { $each : 0 } } }");
    ASSERT_NOT_OK(mod.init(modObj["$addToSet"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal()));

    // An object is not valid after $each
    modObj = fromjson("{ $addToSet : { a : { $each : { a : 1 } } } }");
    ASSERT_NOT_OK(mod.init(modObj["$addToSet"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal()));
}

TEST(Init, ParsesSimple) {
    Mod(fromjson("{ $addToSet : { a : 1 } }"));
    Mod(fromjson("{ $addToSet : { a : 'foo' } }"));
    Mod(fromjson("{ $addToSet : { a : {} } }"));
    Mod(fromjson("{ $addToSet : { a : { x : 1 } } }"));
    Mod(fromjson("{ $addToSet : { a : [] } }"));
    Mod(fromjson("{ $addToSet : { a : [1, 2] } } }"));
    Mod(fromjson("{ $addToSet : { 'a.b' : 1 } }"));
    Mod(fromjson("{ $addToSet : { 'a.b' : 'foo' } }"));
    Mod(fromjson("{ $addToSet : { 'a.b' : {} } }"));
    Mod(fromjson("{ $addToSet : { 'a.b' : { x : 1} } }"));
    Mod(fromjson("{ $addToSet : { 'a.b' : [] } }"));
    Mod(fromjson("{ $addToSet : { 'a.b' : [1, 2] } } }"));
}

TEST(Init, ParsesEach) {
    Mod(fromjson("{ $addToSet : { a : { $each : [] } } }"));
    Mod(fromjson("{ $addToSet : { a : { $each : [ 1 ] } } }"));
    Mod(fromjson("{ $addToSet : { a : { $each : [ 1, 2 ] } } }"));
    Mod(fromjson("{ $addToSet : { a : { $each : [ 1, 2, 1 ] } } }"));
    Mod(fromjson("{ $addToSet : { a : { $each : [ {} ] } } }"));
    Mod(fromjson("{ $addToSet : { a : { $each : [ { x : 1 } ] } } }"));
    Mod(fromjson("{ $addToSet : { a : { $each : [ { x : 1 }, { y : 2 } ] } } }"));
    Mod(fromjson("{ $addToSet : { a : { $each : [ { x : 1 }, { y : 2 }, { x : 1 } ] } } }"));
}

TEST(SimpleMod, PrepareOKTargetNotFound) {
    Document doc(fromjson("{}"));
    Mod mod(fromjson("{ $addToSet : { a : 1 } }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_FALSE(execInfo.noOp);
}

TEST(SimpleMod, PrepareOKTargetFound) {
    Document doc(fromjson("{ a : [ 1 ] }"));
    Mod mod(fromjson("{ $addToSet : { a : 1 } }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_TRUE(execInfo.noOp);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(mod.log(&logBuilder));
    ASSERT_EQUALS(fromjson("{ $set : { a : [ 1 ] } }"), logDoc);
}

TEST(SimpleMod, PrepareInvalidTargetNumber) {
    Document doc(fromjson("{ a : 1 }"));
    Mod mod(fromjson("{ $addToSet : { a : 1 } }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_NOT_OK(mod.prepare(doc.root(), "", &execInfo));
}

TEST(SimpleMod, PrepareInvalidTarget) {
    Document doc(fromjson("{ a : {} }"));
    Mod mod(fromjson("{ $addToSet : { a : 1 } }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_NOT_OK(mod.prepare(doc.root(), "", &execInfo));
}

TEST(SimpleMod, ApplyAndLogEmptyDocument) {
    Document doc(fromjson("{}"));
    Mod mod(fromjson("{ $addToSet : { a : 1 } }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(mod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{ a : [ 1 ] }"), doc);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(mod.log(&logBuilder));
    ASSERT_EQUALS(fromjson("{ $set : { a : [ 1 ] } }"), logDoc);
}

TEST(SimpleMod, ApplyAndLogEmptyArray) {
    Document doc(fromjson("{ a : [] }"));
    Mod mod(fromjson("{ $addToSet : { a : 1 } }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(mod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{ a : [ 1 ] }"), doc);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(mod.log(&logBuilder));
    ASSERT_EQUALS(fromjson("{ $set : { a : [ 1 ] } }"), logDoc);
}

TEST(SimpleEachMod, ApplyAndLogEmptyDocument) {
    Document doc(fromjson("{}"));
    Mod mod(fromjson("{ $addToSet : { a : { $each : [1, 2, 3] } } }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(mod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{ a : [ 1, 2, 3 ] }"), doc);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(mod.log(&logBuilder));
    ASSERT_EQUALS(fromjson("{ $set : { a : [ 1, 2, 3 ] } }"), logDoc);
}

TEST(SimpleEachMod, ApplyAndLogEmptyArray) {
    Document doc(fromjson("{ a : [] }"));
    Mod mod(fromjson("{ $addToSet : { a : { $each : [1, 2, 3] } } }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(mod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{ a : [ 1, 2, 3 ] }"), doc);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(mod.log(&logBuilder));
    ASSERT_EQUALS(fromjson("{ $set : { a : [ 1, 2, 3 ] } }"), logDoc);
}

TEST(SimpleMod, ApplyAndLogPopulatedArray) {
    Document doc(fromjson("{ a : [ 'x' ] }"));
    Mod mod(fromjson("{ $addToSet : { a : 1 } }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(mod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{ a : [ 'x', 1 ] }"), doc);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(mod.log(&logBuilder));
    ASSERT_EQUALS(fromjson("{ $set : { a : [ 'x', 1 ] } }"), logDoc);
}

TEST(SimpleEachMod, ApplyAndLogPopulatedArray) {
    Document doc(fromjson("{ a : [ 'x' ] }"));
    Mod mod(fromjson("{ $addToSet : { a : { $each : [1, 2, 3] } } }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(mod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{ a : [ 'x', 1, 2, 3 ] }"), doc);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(mod.log(&logBuilder));
    ASSERT_EQUALS(fromjson("{ $set : { a : [ 'x', 1, 2, 3 ] } }"), logDoc);
}

TEST(NoOp, AddOneExistingIsNoOp) {
    Document doc(fromjson("{ a : [ 1, 2, 3 ] }"));
    Mod mod(fromjson("{ $addToSet : { a : 1 } }"));
    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_TRUE(execInfo.noOp);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(mod.log(&logBuilder));
    ASSERT_EQUALS(fromjson("{ $set : { a : [ 1, 2, 3 ] } }"), logDoc);
}

TEST(NoOp, AddSeveralExistingIsNoOp) {
    Document doc(fromjson("{ a : [ 1, 2, 3 ] }"));
    Mod mod(fromjson("{ $addToSet : { a : { $each : [1, 2] } } }"));
    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_TRUE(execInfo.noOp);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(mod.log(&logBuilder));
    ASSERT_EQUALS(fromjson("{ $set : { a : [ 1, 2, 3 ] } }"), logDoc);
}

TEST(NoOp, AddAllExistingIsNoOp) {
    Document doc(fromjson("{ a : [ 1, 2, 3 ] }"));
    Mod mod(fromjson("{ $addToSet : { a : { $each : [1, 2, 3] } } }"));
    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_TRUE(execInfo.noOp);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(mod.log(&logBuilder));
    ASSERT_EQUALS(fromjson("{ $set : { a : [ 1, 2, 3 ] } }"), logDoc);
}

TEST(Deduplication, ExistingDuplicatesArePreserved) {
    Document doc(fromjson("{ a : [ 1, 1, 2, 1, 2, 2 ] }"));
    Mod mod(fromjson("{ $addToSet : { a : 3 } }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(mod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{ a : [ 1, 1, 2, 1, 2, 2, 3] }"), doc);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(mod.log(&logBuilder));
    ASSERT_EQUALS(fromjson("{ $set : { a : [ 1, 1, 2, 1, 2, 2, 3] } }"), logDoc);
}

TEST(Deduplication, NewDuplicatesAreElided) {
    Document doc(fromjson("{ a : [ 1, 1, 2, 1, 2, 2 ] }"));
    Mod mod(fromjson("{ $addToSet : { a : { $each : [ 4, 1, 3, 2, 3, 1, 3, 3, 2, 4] } } }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(mod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{ a : [ 1, 1, 2, 1, 2, 2, 4, 3] }"), doc);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(mod.log(&logBuilder));
    ASSERT_EQUALS(fromjson("{ $set : { a : [ 1, 1, 2, 1, 2, 2, 4, 3] } }"), logDoc);
}

TEST(Regressions, SERVER_12848) {
    // Proof that the mod works ok (the real issue was in validate).

    Document doc(fromjson("{ _id : 1, a : [ 1, [ ] ] }"));
    Mod mod(fromjson("{ $addToSet : { 'a.1' : 1 } }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.1");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(mod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{ _id : 1, a : [ 1, [ 1 ] ] }"), doc);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(mod.log(&logBuilder));
    ASSERT_EQUALS(fromjson("{ $set : { 'a.1' : [ 1 ] } }"), logDoc);
}

TEST(Deduplication, DeduplicationRespectsCollation) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kToLowerString);
    Document doc(fromjson("{ a : ['bar'] }"));
    Mod mod(fromjson("{ $addToSet : { a : { $each : ['FOO', 'foo'] } } }"));
    mod.mod().setCollator(&collator);

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));

    ASSERT_FALSE(execInfo.noOp);
    ASSERT_OK(mod.apply());

    ASSERT(doc.compareWithBSONObj(fromjson("{ a : ['bar', 'FOO'] }"), false) == 0 ||
           doc.compareWithBSONObj(fromjson("{ a: ['bar', 'foo'] }"), false) == 0);
}

TEST(Deduplication, ExistingDuplicatesArePreservedWithRespectToCollation) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kToLowerString);
    Document doc(fromjson("{ a : ['bar', 'BAR'] }"));
    Mod mod(fromjson("{ $addToSet : { a : { $each : ['FOO', 'foo'] } } }"));
    mod.mod().setCollator(&collator);

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));

    ASSERT_FALSE(execInfo.noOp);
    ASSERT_OK(mod.apply());

    ASSERT_EQUALS(doc, fromjson("{ a : ['bar', 'BAR', 'FOO'] }"));
}

TEST(Collation, AddToSetRespectsCollationFromModifierOptions) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    Document doc(fromjson("{ a : ['not'] }"));
    Mod mod(fromjson("{ $addToSet : { a : 'equal' } }"),
            ModifierInterface::Options::normal(&collator));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));

    ASSERT_TRUE(execInfo.noOp);
}

TEST(Collation, AddToSetRespectsCollationFromSetCollation) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    Document doc(fromjson("{ a : ['not'] }"));
    Mod mod(fromjson("{ $addToSet : { a : 'equal' } }"));
    mod.mod().setCollator(&collator);

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));

    ASSERT_TRUE(execInfo.noOp);
}

TEST(Collation, AddToSetWithEachRespectsCollation) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kToLowerString);
    Document doc(fromjson("{ a : ['abc'] }"));
    Mod mod(fromjson("{ $addToSet : { a : { $each : ['ABC', 'bdc'] } } }"));
    mod.mod().setCollator(&collator);

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));

    ASSERT_FALSE(execInfo.noOp);
    ASSERT_OK(mod.apply());

    ASSERT_EQUALS(doc, fromjson("{ a : ['abc', 'bdc'] }"));
}
}  // namespace
