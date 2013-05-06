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
 */


#include "mongo/db/ops/modifier_set.h"

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/mutable_bson_test_utils.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/platform/cstdint.h"
#include "mongo/unittest/unittest.h"

namespace {

    using mongo::BSONObj;
    using mongo::fromjson;
    using mongo::ModifierInterface;
    using mongo::NumberInt;
    using mongo::ModifierSet;
    using mongo::Status;
    using mongo::StringData;
    using mongo::mutablebson::checkDoc;
    using mongo::mutablebson::ConstElement;
    using mongo::mutablebson::countChildren;
    using mongo::mutablebson::Document;
    using mongo::mutablebson::Element;

    /** Helper to build and manipulate a $set mod. */
    class Mod {
    public:
        Mod() : _mod() {}

        explicit Mod(BSONObj modObj) {
            _modObj = modObj;
            ASSERT_OK(_mod.init(_modObj["$set"].embeddedObject().firstElement()));
        }

        Status prepare(Element root,
                       const StringData& matchedField,
                       ModifierInterface::ExecInfo* execInfo) {
            return _mod.prepare(root, matchedField, execInfo);
        }

        Status apply() const {
            return _mod.apply();
        }

        Status log(Element logRoot) const {
            return _mod.log(logRoot);
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
        ASSERT_NOT_OK(mod.init(modObj["$set"].embeddedObject().firstElement()));
    }

    TEST(Init, NotOkForStorage) {
        BSONObj modObj = fromjson("{$set: {a: {$inc: {b: 1}}}}");
        ModifierSet mod;
        ASSERT_NOT_OK(mod.init(modObj["$set"].embeddedObject().firstElement()));
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
        ASSERT_TRUE(execInfo.inPlace);
        ASSERT_TRUE(execInfo.noOp);
    }

    TEST(SimpleMod, PrepareApplyEmptyDocument) {
        Document doc(fromjson("{}"));
        Mod setMod(fromjson("{$set: {a: 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_FALSE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson("{a: 2}")));
    }

    TEST(SimpleMod, PrepareApplyInPlace) {
        Document doc(fromjson("{a: 1}"));
        Mod setMod(fromjson("{$set: {a: 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_TRUE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson("{a: 2}")));
    }

    TEST(SimpleMod, PrepareApplyOverridePath) {
        Document doc(fromjson("{a: {b: 1}}"));
        Mod setMod(fromjson("{$set: {a: 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_FALSE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson("{a: 2}")));
    }

    TEST(SimpleMod, PrepareApplyChangeType) {
        Document doc(fromjson("{a: 'str'}"));
        Mod setMod(fromjson("{$set: {a: 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_FALSE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson("{a: 2}")));
    }

    TEST(SimpleMod, PrepareApplyNewPath) {
        Document doc(fromjson("{b: 1}"));
        Mod setMod(fromjson("{$set: {a: 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_FALSE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson("{b: 1, a: 2}")));
    }

    TEST(SimpleMod, LogNormal) {
        BSONObj obj = fromjson("{a: 1}");
        Mod setMod(fromjson("{$set: {a: 2}}"));

        Document doc(obj);
        ModifierInterface::ExecInfo dummy;
        ASSERT_OK(setMod.prepare(doc.root(), "", &dummy));

        Document logDoc;
        ASSERT_OK(setMod.log(logDoc.root()));
        ASSERT_EQUALS(countChildren(logDoc.root()), 1u);
        ASSERT_TRUE(checkDoc(logDoc, fromjson("{$set: {a: 2}}")));
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
        ASSERT_TRUE(execInfo.inPlace);
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
        ASSERT_TRUE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson(("{a: {b: 2}}"))));
    }

    TEST(DottedMod, PrepareApplyChangeType) {
        Document doc(fromjson("{a: {b: 'str'}}"));
        Mod setMod(fromjson("{$set: {'a.b': 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.b");
        ASSERT_FALSE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson(("{a: {b: 2}}"))));
    }

    TEST(DottedMod, PrepareApplyChangePath) {
        Document doc(fromjson("{a: {b: {c: 1}}}"));
        Mod setMod(fromjson("{$set: {'a.b': 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.b");
        ASSERT_FALSE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson(("{a: {b: 2}}"))));
    }

    TEST(DottedMod, PrepareApplyExtendPath) {
        Document doc(fromjson("{a: {c: 1}}"));
        Mod setMod(fromjson("{$set: {'a.b': 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.b");
        ASSERT_FALSE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson(("{a: {c: 1, b: 2}}"))));
    }

    TEST(DottedMod, PrepareApplyNewPath) {
        Document doc(fromjson("{c: 1}"));
        Mod setMod(fromjson("{$set: {'a.b': 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.b");
        ASSERT_FALSE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson("{c: 1, a: {b: 2}}")));
    }

    TEST(DottedMod, PrepareApplyEmptyDoc) {
        Document doc(fromjson("{}"));
        Mod setMod(fromjson("{$set: {'a.b': 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.b");
        ASSERT_FALSE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson("{a: {b: 2}}")));
    }

    TEST(DottedMod, PrepareApplyFieldWithDot) {
        Document doc(fromjson("{'a.b':4}"));
        Mod setMod(fromjson("{$set: {'a.b': 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.b");
        ASSERT_FALSE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson("{'a.b':4, a: {b: 2}}")));
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
        ASSERT_TRUE(execInfo.inPlace);
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
        ASSERT_TRUE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson("{a: [{b: 0},{b: 1},{b: 2}]}")));
    }

    TEST(IndexedMod, PrepareApplyNormalArray) {
        Document doc(fromjson("{a: [{b: 0},{b: 1}]}"));
        Mod setMod(fromjson("{$set: {'a.2.b': 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.2.b");
        ASSERT_FALSE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson("{a: [{b: 0},{b: 1},{b: 2}]}")));
    }

    TEST(IndexedMod, PrepareApplyPaddingArray) {
        Document doc(fromjson("{a: [{b: 0}]}"));
        Mod setMod(fromjson("{$set: {'a.2.b': 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.2.b");
        ASSERT_FALSE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson("{a: [{b: 0},null,{b: 2}]}")));
    }

    TEST(IndexedMod, PrepareApplyNumericObject) {
        Document doc(fromjson("{a: {b: 0}}"));
        Mod setMod(fromjson("{$set: {'a.2.b': 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.2.b");
        ASSERT_FALSE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson("{a: {b: 0, '2': {b: 2}}}")));
    }

    TEST(IndexedMod, PrepareApplyNumericField) {
        Document doc(fromjson("{a: {'2': {b: 1}}}"));
        Mod setMod(fromjson("{$set: {'a.2.b': 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.2.b");
        ASSERT_TRUE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson("{a: {'2': {b: 2}}}")));
    }

    TEST(IndexedMod, PrepareApplyExtendNumericField) {
        Document doc(fromjson("{a: {'2': {c: 1}}}"));
        Mod setMod(fromjson("{$set: {'a.2.b': 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.2.b");
        ASSERT_FALSE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson("{a: {'2': {c: 1, b: 2}}}")));
    }

    TEST(IndexedMod, PrepareApplyEmptyObject) {
        Document doc(fromjson("{a: {}}"));
        Mod setMod(fromjson("{$set: {'a.2.b': 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.2.b");
        ASSERT_FALSE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson("{a: {'2': {b: 2}}}")));
    }

    TEST(IndexedMod, PrepareApplyEmptyArray) {
        Document doc(fromjson("{a: []}"));
        Mod setMod(fromjson("{$set: {'a.2.b': 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.2.b");
        ASSERT_FALSE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson("{a: [null, null, {b: 2}]}")));
    }

    TEST(IndexedMod, PrepareApplyInexistent) {
        Document doc(fromjson("{}"));
        Mod setMod(fromjson("{$set: {'a.2.b': 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.2.b");
        ASSERT_FALSE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson("{a: {'2': {b: 2}}}")));
    }

    TEST(IndexedMod, LogNormal) {
        BSONObj obj = fromjson("{a: [{b:0}, {b:1}]}");
        Document doc(obj);
        Mod setMod(fromjson("{$set: {'a.2.b': 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        Document logDoc;
        BSONObj logObj = fromjson("{$set: {'a.2.b': 2}}");
        ASSERT_OK(setMod.log(logDoc.root()));
        ASSERT_EQUALS(countChildren(logDoc.root()), 1u);
        ASSERT_TRUE(checkDoc(logDoc, logObj));
    }

    TEST(IndexedMod, LogEmptyArray) {
        BSONObj obj = fromjson("{a: []}");
        Document doc(obj);
        Mod setMod(fromjson("{$set: {'a.2.b': 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        Document logDoc;
        BSONObj logObj = fromjson("{$set: {'a.2.b': 2}}");
        ASSERT_OK(setMod.log(logDoc.root()));
        ASSERT_EQUALS(countChildren(logDoc.root()), 1u);
        ASSERT_TRUE(checkDoc(logDoc, logObj));
    }

    TEST(IndexedMod, LogEmptyObject) {
        BSONObj obj = fromjson("{a: []}");
        Document doc(obj);
        Mod setMod(fromjson("{$set: {'a.2.b': 2}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        Document logDoc;
        BSONObj logObj = fromjson("{$set: {'a.2.b': 2}}");
        ASSERT_OK(setMod.log(logDoc.root()));
        ASSERT_EQUALS(countChildren(logDoc.root()), 1u);
        ASSERT_TRUE(checkDoc(logDoc, logObj));
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
        ASSERT_FALSE(execInfo.inPlace);
        ASSERT_TRUE(execInfo.noOp);
    }

    TEST(IndexedComplexMod, PrepareSameStructure) {
        Document doc(fromjson("{a: [{b: {c: 0, d: 0}}, {b: {c: 1, xxx: 1}}]}}"));
        Mod setMod(fromjson("{$set: {'a.1.b': {c: 1, d: 1}}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.1.b");
        ASSERT_FALSE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);
    }

} // unnamed namespace
