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


#include "mongo/db/ops/modifier_unset.h"

#include <cstdint>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/mutable_bson_test_utils.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/ops/log_builder.h"
#include "mongo/unittest/unittest.h"

namespace {

using mongo::Array;
using mongo::BSONObj;
using mongo::fromjson;
using mongo::LogBuilder;
using mongo::ModifierInterface;
using mongo::ModifierUnset;
using mongo::Status;
using mongo::StringData;
using mongo::mutablebson::Document;
using mongo::mutablebson::Element;

/** Helper to build and manipulate a $set mod. */
class Mod {
public:
    Mod() : _mod() {}

    explicit Mod(BSONObj modObj) {
        _modObj = modObj;
        ASSERT_OK(_mod.init(_modObj["$unset"].embeddedObject().firstElement(),
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

    ModifierUnset& mod() {
        return _mod;
    }

    BSONObj modObj() {
        return _modObj;
    }

private:
    ModifierUnset _mod;
    BSONObj _modObj;
};

//
// Simple mod
//

TEST(SimpleMod, PrepareNoOp) {
    Document doc(fromjson("{}"));
    Mod modUnset(fromjson("{$unset: {a: true}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(modUnset.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_TRUE(execInfo.noOp);
}

TEST(SimpleMod, PrepareApplyNormal) {
    Document doc(fromjson("{a: 1, b: 2}"));
    Mod modUnset(fromjson("{$unset: {a: true}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(modUnset.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(modUnset.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{b: 2}"), doc);
}

TEST(SimpleMod, PrepareApplyInPlace) {
    Document doc(fromjson("{x: 0, a: 1}"));
    Mod modUnset(fromjson("{$unset: {a: true}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(modUnset.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(modUnset.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());  // TODO turn in-place on for this.
    ASSERT_EQUALS(fromjson("{x: 0}"), doc);
}

TEST(SimpleMod, PrepareApplyGeneratesEmptyDocument) {
    Document doc(fromjson("{a: 1}"));
    Mod modUnset(fromjson("{$unset: {a: true}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(modUnset.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(modUnset.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());  // TODO turn in-place on for this.
    ASSERT_EQUALS(fromjson("{}"), doc);
}

TEST(SimpleMod, PrepareApplyUnsetSubtree) {
    Document doc(fromjson("{a: {b: 1}, c: 2}"));
    Mod modUnset(fromjson("{$unset: {a: true}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(modUnset.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(modUnset.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{c: 2}"), doc);
}

TEST(SimpleMod, LogNormal) {
    BSONObj obj = fromjson("{a: 1}");
    Document doc(obj);
    Mod modUnset(fromjson("{$unset: {a: true}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(modUnset.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_FALSE(execInfo.noOp);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(modUnset.log(&logBuilder));
    ASSERT_EQUALS(modUnset.modObj(), logDoc);
}

//
// Dotted mod
//

TEST(DottedMod, PrepareNoOp) {
    Document doc(fromjson("{c:2}"));
    Mod modUnset(fromjson("{$unset: {'a.b': true}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(modUnset.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.b");
    ASSERT_TRUE(execInfo.noOp);
}

TEST(DottedMod, PrepareApplyNormal) {
    Document doc(fromjson("{a: {b: 1}, c: 2}"));
    Mod modUnset(fromjson("{$unset: {'a.b': true}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(modUnset.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.b");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(modUnset.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{a:{}, c:2}"), doc);
}

TEST(DottedMod, PrepareApplyInPlace) {
    Document doc(fromjson("{x: 0, a: {b: 1}}"));
    Mod modUnset(fromjson("{$unset: {'a.b': true}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(modUnset.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.b");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(modUnset.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());  // TODO turn in-place on for this.
    ASSERT_EQUALS(fromjson("{x: 0, a:{}}"), doc);
}

TEST(DottedMod, PrepareApplyUnsetNestedSubobject) {
    Document doc(fromjson("{a: {b: {c: 1}}}"));
    Mod modUnset(fromjson("{$unset: {'a.b': true}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(modUnset.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.b");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(modUnset.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());  // TODO turn in-place on for this.
    ASSERT_EQUALS(fromjson("{a: {}}"), doc);
}

//
// Indexed mod
//

TEST(IndexedMod, PrepareNoOp) {
    Document doc(fromjson("{a:[]}"));
    Mod modUnset(fromjson("{$unset: {'a.0': true}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(modUnset.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.0");
    ASSERT_TRUE(execInfo.noOp);
}

TEST(IndexedMod, PrepareApplyNormal) {
    Document doc(fromjson("{a:[0,1,2]}"));
    Mod modUnset(fromjson("{$unset: {'a.0': true}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(modUnset.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.0");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(modUnset.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{a:[null,1,2]}"), doc);
}

TEST(IndexedMod, PrepareApplyInPlace) {
    Document doc(fromjson("{b:1, a:[1]}"));
    Mod modUnset(fromjson("{$unset: {'a.0': true}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(modUnset.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.0");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(modUnset.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());  // TODO turn in-place on for this.
    ASSERT_EQUALS(fromjson("{b:1, a:[null]}"), doc);
}

TEST(IndexedMod, PrepareApplyInPlaceNuance) {
    // Can't change the encoding in the middle of a bson stream.
    Document doc(fromjson("{a:[1], b:1}"));
    Mod modUnset(fromjson("{$unset: {'a.0': true}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(modUnset.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.0");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(modUnset.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{a:[null], b:1}"), doc);
}

TEST(IndexedMod, PrepareApplyInnerObject) {
    Document doc(fromjson("{a:[{b:1}]}"));
    Mod modUnset(fromjson("{$unset: {'a.0.b': true}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(modUnset.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.0.b");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(modUnset.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{a:[{}]}"), doc);
}

TEST(IndexedMod, PrepareApplyObject) {
    Document doc(fromjson("{a:[{b:1}]}"));
    Mod modUnset(fromjson("{$unset: {'a.0': true}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(modUnset.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.0");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(modUnset.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{a:[null]}"), doc);
}

TEST(IndexedMod, LogNormal) {
    Document doc(fromjson("{a:[0,1,2]}"));
    Mod modUnset(fromjson("{$unset: {'a.0': true}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(modUnset.prepare(doc.root(), "", &execInfo));

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(modUnset.log(&logBuilder));
    ASSERT_EQUALS(modUnset.modObj(), logDoc);
}

//
// Positional mod
//

TEST(PositionalMod, PrepareNoOp) {
    Document doc(fromjson("{a:[{b:0}]}"));
    Mod modUnset(fromjson("{$unset: {'a.$.b': true}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(modUnset.prepare(doc.root(), "1", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.1.b");
    ASSERT_TRUE(execInfo.noOp);
}

TEST(PositionalMod, PrepareMissingPositional) {
    Document doc(fromjson("{a:[{b:0},{c:1}]}"));
    Mod modUnset(fromjson("{$unset: {'a.$.b': true}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_NOT_OK(modUnset.prepare(doc.root(), "" /* no position */, &execInfo));
}

TEST(PositionalMod, PrepareApplyNormal) {
    Document doc(fromjson("{a:[{b:0},{c:1}]}"));
    Mod modUnset(fromjson("{$unset: {'a.$.b': true}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(modUnset.prepare(doc.root(), "0", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.0.b");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(modUnset.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{a: [{}, {c:1}]}"), doc);
}

TEST(PositionalMod, PrepareApplyObject) {
    Document doc(fromjson("{a:[{b:0},{c:1}]}"));
    Mod modUnset(fromjson("{$unset: {'a.$': true}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(modUnset.prepare(doc.root(), "0", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.0");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(modUnset.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{a: [null, {c:1}]}"), doc);
}

TEST(PositionalMod, PrepareApplyInPlace) {
    Document doc(fromjson("{b:1, a:[{b:1}]}"));
    Mod modUnset(fromjson("{$unset: {'a.$.b': true}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(modUnset.prepare(doc.root(), "0", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.0.b");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(modUnset.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());  // TODO turn in-place on for this.
    ASSERT_EQUALS(fromjson("{b:1, a:[{}]}"), doc);
}

TEST(PositionalMod, LogNormal) {
    Document doc(fromjson("{b:1, a:[{b:1}]}"));
    Mod modUnset(fromjson("{$unset: {'a.$.b': true}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(modUnset.prepare(doc.root(), "0", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.0.b");
    ASSERT_FALSE(execInfo.noOp);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(modUnset.log(&logBuilder));
    ASSERT_EQUALS(fromjson("{$unset: {'a.0.b': true}}"), logDoc);
}

TEST(LegacyData, CanUnsetInvalidField) {
    Document doc(fromjson("{b:1, a:[{$b:1}]}"));
    Mod modUnset(fromjson("{$unset: {'a.$.$b': true}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(modUnset.prepare(doc.root(), "0", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.0.$b");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(modUnset.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{b:1, a:[{}]}"), doc);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(modUnset.log(&logBuilder));
    ASSERT_EQUALS(fromjson("{$unset: {'a.0.$b': true}}"), logDoc);
}


}  // unnamed namespace
