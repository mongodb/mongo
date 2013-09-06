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


#include "mongo/db/ops/modifier_set.h"

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/mutable_bson_test_utils.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/ops/log_builder.h"
#include "mongo/platform/cstdint.h"
#include "mongo/unittest/unittest.h"

namespace {

    using mongo::BSONObj;
    using mongo::fromjson;
    using mongo::LogBuilder;
    using mongo::ModifierInterface;
    using mongo::NumberInt;
    using mongo::ModifierSet;
    using mongo::Status;
    using mongo::StringData;
    using mongo::mutablebson::ConstElement;
    using mongo::mutablebson::countChildren;
    using mongo::mutablebson::Document;
    using mongo::mutablebson::Element;

    /** Helper to build and manipulate a $set mod. */
    class Mod {
    public:
        Mod() : _mod() {}

        explicit Mod(BSONObj modObj, bool fromRepl = false) :
                _mod(mongoutils::str::equals(modObj.firstElement().fieldName(), "$setOnInsert") ?
                ModifierSet::SET_ON_INSERT : ModifierSet::SET_NORMAL) {

            _modObj = modObj;
            const StringData& modName = modObj.firstElement().fieldName();
            ASSERT_OK(_mod.init(_modObj[modName].embeddedObject().firstElement(),
                                !fromRepl ? ModifierInterface::Options::normal():
                                            ModifierInterface::Options::fromRepl()));
        }

        Status prepare(Element root,
                       const StringData& matchedField,
                       ModifierInterface::ExecInfo* execInfo) {
            return _mod.prepare(root, matchedField, execInfo);
        }

        Status apply() const {
            return _mod.apply();
        }

        Status log(LogBuilder* logBuilder) const {
            return _mod.log(logBuilder);
        }

        ModifierSet& mod() { return _mod; }

    private:
        ModifierSet _mod;
        BSONObj _modObj;
    };

    //
    // Init tests
    //

    TEST(Init, EmptyOperation) {
        BSONObj modObj = fromjson("{$set: {}}");
        ModifierSet mod;
        ASSERT_NOT_OK(mod.init(modObj["$set"].embeddedObject().firstElement(),
                               ModifierInterface::Options::normal() ));
    }

    TEST(Init, NotOkForStorage) {
        BSONObj modObj = fromjson("{$set: {a: {$inc: {b: 1}}}}");
        ModifierSet mod;
        ASSERT_NOT_OK(mod.init(modObj["$set"].embeddedObject().firstElement(),
                               ModifierInterface::Options::normal() ));
    }

    //
    // Simple Mods
    //

    TEST(SimpleMod, PrepareNoOp) {
        Document doc(fromjson("{a: 2}"));
        Mod setMod(fromjson("{$set: {a: 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_TRUE(execInfo.noOp);
    }

    TEST(SimpleMod, PrepareSetOnInsert) {
        Document doc(fromjson("{a: 1}"));
        Mod setMod(fromjson("{$setOnInsert: {a: 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_FALSE(execInfo.noOp);
        ASSERT_EQUALS(execInfo.context, ModifierInterface::ExecInfo::INSERT_CONTEXT);
    }

    TEST(SimpleMod, PrepareApplyEmptyDocument) {
        Document doc(fromjson("{}"));
        Mod setMod(fromjson("{$set: {a: 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson("{a: 2}"), doc);
    }

    TEST(SimpleMod, PrepareApplyInPlace) {
        Document doc(fromjson("{a: 1}"));
        Mod setMod(fromjson("{$set: {a: 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_TRUE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson("{a: 2}"), doc);
    }

    TEST(SimpleMod, PrepareApplyOverridePath) {
        Document doc(fromjson("{a: {b: 1}}"));
        Mod setMod(fromjson("{$set: {a: 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson("{a: 2}"), doc);
    }

    TEST(SimpleMod, PrepareApplyChangeType) {
        Document doc(fromjson("{a: 'str'}"));
        Mod setMod(fromjson("{$set: {a: 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson("{a: 2}"), doc);
    }

    TEST(SimpleMod, PrepareApplyNewPath) {
        Document doc(fromjson("{b: 1}"));
        Mod setMod(fromjson("{$set: {a: 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson("{b: 1, a: 2}"), doc);
    }

    TEST(SimpleMod, LogNormal) {
        BSONObj obj = fromjson("{a: 1}");
        Mod setMod(fromjson("{$set: {a: 2}}"));

        Document doc(obj);
        ModifierInterface::ExecInfo dummy;
        ASSERT_OK(setMod.prepare(doc.root(), "", &dummy));

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(setMod.log(&logBuilder));
        ASSERT_EQUALS(countChildren(logDoc.root()), 1u);
        ASSERT_EQUALS(fromjson("{$set: {a: 2}}"), logDoc);
    }

    //
    // Simple dotted mod
    //

    TEST(DottedMod, PrepareNoOp) {
        Document doc(fromjson("{a: {b: 2}}"));
        Mod setMod(fromjson("{$set: {'a.b': 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.b");
        ASSERT_TRUE(execInfo.noOp);
    }

    TEST(DottedMod, PreparePathNotViable) {
        Document doc(fromjson("{a:1}"));
        Mod setMod(fromjson("{$set: {'a.b': 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_NOT_OK(setMod.prepare(doc.root(), "", &execInfo));
    }

    TEST(DottedMod, PreparePathNotViableArrray) {
        Document doc(fromjson("{a:[{b:1}]}"));
        Mod setMod(fromjson("{$set: {'a.b': 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_NOT_OK(setMod.prepare(doc.root(), "", &execInfo));
    }

    TEST(DottedMod, PrepareApplyInPlace) {
        Document doc(fromjson("{a: {b: 1}}"));
        Mod setMod(fromjson("{$set: {'a.b': 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.b");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_TRUE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson("{a: {b: 2}}"), doc);
    }

    TEST(DottedMod, PrepareApplyChangeType) {
        Document doc(fromjson("{a: {b: 'str'}}"));
        Mod setMod(fromjson("{$set: {'a.b': 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.b");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson("{a: {b: 2}}"), doc);
    }

    TEST(DottedMod, PrepareApplyChangePath) {
        Document doc(fromjson("{a: {b: {c: 1}}}"));
        Mod setMod(fromjson("{$set: {'a.b': 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.b");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson("{a: {b: 2}}"), doc);
    }

    TEST(DottedMod, PrepareApplyExtendPath) {
        Document doc(fromjson("{a: {c: 1}}"));
        Mod setMod(fromjson("{$set: {'a.b': 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.b");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson("{a: {c: 1, b: 2}}"), doc);
    }

    TEST(DottedMod, PrepareApplyNewPath) {
        Document doc(fromjson("{c: 1}"));
        Mod setMod(fromjson("{$set: {'a.b': 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.b");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson("{c: 1, a: {b: 2}}"), doc);
    }

    TEST(DottedMod, PrepareApplyEmptyDoc) {
        Document doc(fromjson("{}"));
        Mod setMod(fromjson("{$set: {'a.b': 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.b");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson("{a: {b: 2}}"), doc);
    }

    TEST(DottedMod, PrepareApplyFieldWithDot) {
        Document doc(fromjson("{'a.b':4}"));
        Mod setMod(fromjson("{$set: {'a.b': 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.b");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson("{'a.b':4, a: {b: 2}}"), doc);
    }

    //
    // Indexed mod
    //

    TEST(IndexedMod, PrepareNoOp) {
        Document doc(fromjson("{a: [{b: 0},{b: 1},{b: 2}]}"));
        Mod setMod(fromjson("{$set: {'a.2.b': 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.2.b");
        ASSERT_TRUE(execInfo.noOp);
    }

    TEST(IndexedMod, PrepareNonViablePath) {
        Document doc(fromjson("{a: 0}"));
        Mod setMod(fromjson("{$set: {'a.2.b': 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_NOT_OK(setMod.prepare(doc.root(), "", &execInfo));
    }

    TEST(IndexedMod, PrepareApplyInPlace) {
        Document doc(fromjson("{a: [{b: 0},{b: 1},{b: 1}]}"));
        Mod setMod(fromjson("{$set: {'a.2.b': 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.2.b");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_TRUE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson("{a: [{b: 0},{b: 1},{b: 2}]}"), doc);
    }

    TEST(IndexedMod, PrepareApplyNormalArray) {
        Document doc(fromjson("{a: [{b: 0},{b: 1}]}"));
        Mod setMod(fromjson("{$set: {'a.2.b': 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.2.b");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson("{a: [{b: 0},{b: 1},{b: 2}]}"), doc);
    }

    TEST(IndexedMod, PrepareApplyPaddingArray) {
        Document doc(fromjson("{a: [{b: 0}]}"));
        Mod setMod(fromjson("{$set: {'a.2.b': 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.2.b");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson("{a: [{b: 0},null,{b: 2}]}"), doc);
    }

    TEST(IndexedMod, PrepareApplyNumericObject) {
        Document doc(fromjson("{a: {b: 0}}"));
        Mod setMod(fromjson("{$set: {'a.2.b': 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.2.b");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson("{a: {b: 0, '2': {b: 2}}}"), doc);
    }

    TEST(IndexedMod, PrepareApplyNumericField) {
        Document doc(fromjson("{a: {'2': {b: 1}}}"));
        Mod setMod(fromjson("{$set: {'a.2.b': 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.2.b");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_TRUE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson("{a: {'2': {b: 2}}}"), doc);
    }

    TEST(IndexedMod, PrepareApplyExtendNumericField) {
        Document doc(fromjson("{a: {'2': {c: 1}}}"));
        Mod setMod(fromjson("{$set: {'a.2.b': 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.2.b");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson("{a: {'2': {c: 1, b: 2}}}"), doc);
    }

    TEST(IndexedMod, PrepareApplyEmptyObject) {
        Document doc(fromjson("{a: {}}"));
        Mod setMod(fromjson("{$set: {'a.2.b': 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.2.b");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson("{a: {'2': {b: 2}}}"), doc);
    }

    TEST(IndexedMod, PrepareApplyEmptyArray) {
        Document doc(fromjson("{a: []}"));
        Mod setMod(fromjson("{$set: {'a.2.b': 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.2.b");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson("{a: [null, null, {b: 2}]}"), doc);
    }

    TEST(IndexedMod, PrepareApplyInexistent) {
        Document doc(fromjson("{}"));
        Mod setMod(fromjson("{$set: {'a.2.b': 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.2.b");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson("{a: {'2': {b: 2}}}"), doc);
    }

    TEST(IndexedMod, LogNormal) {
        BSONObj obj = fromjson("{a: [{b:0}, {b:1}]}");
        Document doc(obj);
        Mod setMod(fromjson("{$set: {'a.2.b': 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(setMod.log(&logBuilder));
        ASSERT_EQUALS(countChildren(logDoc.root()), 1u);
        ASSERT_EQUALS(fromjson("{$set: {'a.2.b': 2}}"), logDoc);
    }

    TEST(IndexedMod, LogEmptyArray) {
        BSONObj obj = fromjson("{a: []}");
        Document doc(obj);
        Mod setMod(fromjson("{$set: {'a.2.b': 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(setMod.log(&logBuilder));
        ASSERT_EQUALS(countChildren(logDoc.root()), 1u);
        ASSERT_EQUALS(fromjson("{$set: {'a.2.b': 2}}"), logDoc);
    }

    TEST(IndexedMod, LogEmptyObject) {
        BSONObj obj = fromjson("{a: []}");
        Document doc(obj);
        Mod setMod(fromjson("{$set: {'a.2.b': 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(setMod.log(&logBuilder));
        ASSERT_EQUALS(countChildren(logDoc.root()), 1u);
        ASSERT_EQUALS(fromjson("{$set: {'a.2.b': 2}}"), logDoc);
    }

    //
    // Indexed complex mod
    //

    TEST(IndexedComplexMod, PrepareNoOp) {
        Document doc(fromjson("{a: [{b: {c: 0, d: 0}}, {b: {c: 1, d: 1}}]}}"));
        Mod setMod(fromjson("{$set: {'a.1.b': {c: 1, d: 1}}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.1.b");
        ASSERT_TRUE(execInfo.noOp);
    }

    TEST(IndexedComplexMod, PrepareSameStructure) {
        Document doc(fromjson("{a: [{b: {c: 0, d: 0}}, {b: {c: 1, xxx: 1}}]}}"));
        Mod setMod(fromjson("{$set: {'a.1.b': {c: 1, d: 1}}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.1.b");
        ASSERT_FALSE(execInfo.noOp);
    }

    //
    // Replication version where viable paths don't block modification
    //
    TEST(NonViablePathWithoutRepl, ControlRun) {
        Document doc(fromjson("{a: 1}"));
        Mod setMod(fromjson("{$set: {'a.1.b': 1}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_NOT_OK(setMod.prepare(doc.root(), "", &execInfo));
    }

    TEST(NonViablePathWithRepl, SingleField) {
        Document doc(fromjson("{_id:1, a: 1}"));
        Mod setMod(fromjson("{$set: {'a.1.b': 1}}"), true);

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.1.b");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson("{_id:1, a: {'1': {b: 1}}}"), doc);
    }

    TEST(NonViablePathWithRepl, SingleFieldNoId) {
        Document doc(fromjson("{a: 1}"));
        Mod setMod(fromjson("{$set: {'a.1.b': 1}}"), true);

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.1.b");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson("{a: {'1': {b: 1}}}"), doc);
    }

    TEST(NonViablePathWithRepl, NestedField) {
        Document doc(fromjson("{_id:1, a: {a: 1}}"));
        Mod setMod(fromjson("{$set: {'a.a.1.b': 1}}"), true);

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.a.1.b");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson("{_id:1, a: {a: {'1': {b: 1}}}}"), doc);
    }

    TEST(NonViablePathWithRepl, DoubleNestedField) {
        Document doc(fromjson("{_id:1, a: {b: {c: 1}}}"));
        Mod setMod(fromjson("{$set: {'a.b.c.d': 2}}"), true);

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.b.c.d");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson("{_id:1, a: {b: {c: {d: 2}}}}"), doc);
    }

    TEST(NonViablePathWithRepl, NestedFieldNoId) {
        Document doc(fromjson("{a: {a: 1}}"));
        Mod setMod(fromjson("{$set: {'a.a.1.b': 1}}"), true);

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.a.1.b");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson("{a: {a: {'1': {b: 1}}}}"), doc);
    }

    TEST(NonViablePathWithRepl, ReplayArrayFieldNotAppendedItermediate) {
        Document doc(fromjson("{_id: 0, a: [1, {b: [1]}]}"));
        Mod setMod(fromjson("{$set: {'a.0.b': [0,2]}}}"), true);

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.0.b");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson("{_id: 0, a: [{b: [0,2]}, {b: [1]}]}"), doc);
    }

    // Cases from users/issues/jstests
    TEST(JsTestIssues, Set6) {
        Document doc(fromjson("{_id: 1, r: {a:1, b:2}}"));
        Mod setMod(fromjson("{$set: {'r.a': 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "r.a");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_TRUE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson("{_id: 1, r: {a:2, b:2}}"), doc);

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(setMod.log(&logBuilder));
        ASSERT_EQUALS(countChildren(logDoc.root()), 1u);
        ASSERT_EQUALS(fromjson("{$set: {'r.a': 2}}"), logDoc);
    }

    // Test which failed before and is here to verify correct execution.
    TEST(JsTestIssues, Set6FromRepl) {
        Document doc(fromjson("{_id: 1, r: {a:1, b:2}}"));
        Mod setMod(fromjson("{$set: { 'r.a': 2}}"), true);

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "r.a");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_TRUE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson("{_id: 1, r: {a:2, b:2} }"), doc);

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(setMod.log(&logBuilder));
        ASSERT_EQUALS(countChildren(logDoc.root()), 1u);
        ASSERT_EQUALS(fromjson("{$set: {'r.a': 2}}"), logDoc);
    }

} // unnamed namespace
