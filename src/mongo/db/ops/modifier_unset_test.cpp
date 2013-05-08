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


#include "mongo/db/ops/modifier_unset.h"

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

    using mongo::Array;
    using mongo::BSONObj;
    using mongo::fromjson;
    using mongo::ModifierInterface;
    using mongo::ModifierUnset;
    using mongo::Status;
    using mongo::StringData;
    using mongo::mutablebson::checkDoc;
    using mongo::mutablebson::Document;
    using mongo::mutablebson::Element;

    /** Helper to build and manipulate a $set mod. */
    class Mod {
    public:
        Mod() : _mod() {}

        explicit Mod(BSONObj modObj) {
            _modObj = modObj;
            ASSERT_OK(_mod.init(_modObj["$unset"].embeddedObject().firstElement()));
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

        ModifierUnset& mod() { return _mod; }

        BSONObj modObj() { return _modObj; }

    private:
        ModifierUnset _mod;
        BSONObj _modObj;
    };

    //
    // Simple mod
    //

    TEST(SimpleMod, PrepareNoOp) {
        Document doc(fromjson("{}"));
        Mod modUnset(fromjson("{$unset: {a: 1}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(modUnset.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_TRUE(execInfo.inPlace);
        ASSERT_TRUE(execInfo.noOp);
    }

    TEST(SimpleMod, PrepareApplyNormal) {
        Document doc(fromjson("{a: 1, b: 2}"));
        Mod modUnset(fromjson("{$unset: {a: 1}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(modUnset.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_FALSE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(modUnset.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson("{b: 2}")));
    }

    TEST(SimpleMod, PrepareApplyInPlace) {
        Document doc(fromjson("{x: 0, a: 1}"));
        Mod modUnset(fromjson("{$unset: {a: 1}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(modUnset.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_FALSE(execInfo.inPlace); // TODO turn in-place on for this.
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(modUnset.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson("{x: 0}")));
    }

    TEST(SimpleMod, PrepareApplyGeneratesEmptyDocument) {
        Document doc(fromjson("{a: 1}"));
        Mod modUnset(fromjson("{$unset: {a: 1}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(modUnset.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_FALSE(execInfo.inPlace); // TODO turn in-place on for this.
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(modUnset.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson("{}")));
    }

    TEST(SimpleMod, PrepareApplyUnsetSubtree) {
        Document doc(fromjson("{a: {b: 1}, c: 2}"));
        Mod modUnset(fromjson("{$unset: {a: 1}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(modUnset.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_FALSE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(modUnset.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson("{c: 2}")));
    }

    TEST(SimpleMod, LogNormal) {
        BSONObj obj = fromjson("{a: 1}");
        Document doc(obj);
        Mod modUnset(fromjson("{$unset: {a: 1}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(modUnset.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_FALSE(execInfo.inPlace); // TODO turn in-place on for this.
        ASSERT_FALSE(execInfo.noOp);

        Document logDoc;
        ASSERT_OK(modUnset.log(logDoc.root()));
        ASSERT_TRUE(checkDoc(logDoc, modUnset.modObj()));
    }

    //
    // Dotted mod
    //

    TEST(DottedMod, PrepareNoOp) {
        Document doc(fromjson("{c:2}"));
        Mod modUnset(fromjson("{$unset: {'a.b': 1}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(modUnset.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.b");
        ASSERT_TRUE(execInfo.inPlace);
        ASSERT_TRUE(execInfo.noOp);
    }

    TEST(DottedMod, PrepareApplyNormal) {
        Document doc(fromjson("{a: {b: 1}, c: 2}"));
        Mod modUnset(fromjson("{$unset: {'a.b': 1}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(modUnset.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.b");
        ASSERT_FALSE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(modUnset.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson(("{a:{}, c:2}"))));
    }

    TEST(DottedMod, PrepareApplyInPlace) {
        Document doc(fromjson("{x: 0, a: {b: 1}}"));
        Mod modUnset(fromjson("{$unset: {'a.b': 1}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(modUnset.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.b");
        ASSERT_FALSE(execInfo.inPlace); // TODO turn in-place on for this.
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(modUnset.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson(("{x: 0, a:{}}"))));
    }

    TEST(DottedMod, PrepareApplyUnsetNestedSubobject) {
        Document doc(fromjson("{a: {b: {c: 1}}}"));
        Mod modUnset(fromjson("{$unset: {'a.b': 1}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(modUnset.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.b");
        ASSERT_FALSE(execInfo.inPlace); // TODO turn in-place on for this.
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(modUnset.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson(("{a: {}}"))));
    }

    //
    // Indexed mod
    //

    TEST(IndexedMod, PrepareNoOp) {
        Document doc(fromjson("{a:[]}"));
        Mod modUnset(fromjson("{$unset: {'a.0': 1}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(modUnset.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.0");
        ASSERT_TRUE(execInfo.inPlace);
        ASSERT_TRUE(execInfo.noOp);
    }

    TEST(IndexedMod, PrepareApplyNormal) {
        Document doc(fromjson("{a:[0,1,2]}"));
        Mod modUnset(fromjson("{$unset: {'a.0': 1}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(modUnset.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.0");
        ASSERT_FALSE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(modUnset.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson("{a:[null,1,2]}")));
    }

    TEST(IndexedMod, PrepareApplyInPlace) {
        Document doc(fromjson("{b:1, a:[1]}"));
        Mod modUnset(fromjson("{$unset: {'a.0': 1}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(modUnset.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.0");
        ASSERT_FALSE(execInfo.inPlace); // TODO turn in-place on for this.
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(modUnset.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson("{b:1, a:[null]}")));
    }

    TEST(IndexedMod, PrepareApplyInPlaceNuance) {
        // Can't change the encoding in the middle of a bson stream.
        Document doc(fromjson("{a:[1], b:1}"));
        Mod modUnset(fromjson("{$unset: {'a.0': 1}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(modUnset.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.0");
        ASSERT_FALSE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(modUnset.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson("{a:[null], b:1}")));
    }

    TEST(IndexedMod, PrepareApplyInnerObject) {
        Document doc(fromjson("{a:[{b:1}]}"));
        Mod modUnset(fromjson("{$unset: {'a.0.b': 1}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(modUnset.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.0.b");
        ASSERT_FALSE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(modUnset.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson("{a:[{}]}")));
    }

    TEST(IndexedMod, PrepareApplyObject) {
        Document doc(fromjson("{a:[{b:1}]}"));
        Mod modUnset(fromjson("{$unset: {'a.0': 1}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(modUnset.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.0");
        ASSERT_FALSE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(modUnset.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson("{a:[null]}")));
    }

    TEST(IndexedMod, LogNormal) {
        Document doc(fromjson("{a:[0,1,2]}"));
        Mod modUnset(fromjson("{$unset: {'a.0': 1}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(modUnset.prepare(doc.root(), "", &execInfo));

        Document logDoc;
        ASSERT_OK(modUnset.log(logDoc.root()));
        ASSERT_TRUE(checkDoc(logDoc, modUnset.modObj()));
    }

    //
    // Positional mod
    //

    TEST(PositionalMod, PrepareNoOp) {
        Document doc(fromjson("{a:[{b:0}]}"));
        Mod modUnset(fromjson("{$unset: {'a.$.b': 1}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(modUnset.prepare(doc.root(), "1", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.1.b");
        ASSERT_TRUE(execInfo.inPlace);
        ASSERT_TRUE(execInfo.noOp);
    }

    TEST(PositionalMod, PrepareMissingPositional) {
        Document doc(fromjson("{a:[{b:0},{c:1}]}"));
        Mod modUnset(fromjson("{$unset: {'a.$.b': 1}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_NOT_OK(modUnset.prepare(doc.root(), "" /* no position */, &execInfo));
    }

    TEST(PositionalMod, PrepareApplyNormal) {
        Document doc(fromjson("{a:[{b:0},{c:1}]}"));
        Mod modUnset(fromjson("{$unset: {'a.$.b': 1}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(modUnset.prepare(doc.root(), "0", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.0.b");
        ASSERT_FALSE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(modUnset.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson("{a: [{}, {c:1}]}")));
    }

    TEST(PositionalMod, PrepareApplyObject) {
        Document doc(fromjson("{a:[{b:0},{c:1}]}"));
        Mod modUnset(fromjson("{$unset: {'a.$': 1}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(modUnset.prepare(doc.root(), "0", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.0");
        ASSERT_FALSE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(modUnset.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson("{a: [null, {c:1}]}")));
    }

    TEST(PositionalMod, PrepareApplyInPlace) {
        Document doc(fromjson("{b:1, a:[{b:1}]}"));
        Mod modUnset(fromjson("{$unset: {'a.$.b': 1}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(modUnset.prepare(doc.root(), "0", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.0.b");
        ASSERT_FALSE(execInfo.inPlace); // TODO turn in-place on for this.
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(modUnset.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson("{b:1, a:[{}]}")));
    }

    TEST(PositionalMod, LogNormal) {
        Document doc(fromjson("{b:1, a:[{b:1}]}"));
        Mod modUnset(fromjson("{$unset: {'a.$.b': 1}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(modUnset.prepare(doc.root(), "0", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.0.b");
        ASSERT_FALSE(execInfo.inPlace); // TODO turn in-place on for this.
        ASSERT_FALSE(execInfo.noOp);

        Document logDoc;
        ASSERT_OK(modUnset.log(logDoc.root()));
        ASSERT_TRUE(checkDoc(logDoc, fromjson("{$unset: {'a.0.b': 1}}")));
    }


} // unnamed namespace
