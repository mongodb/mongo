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

#include "mongo/db/ops/modifier_rename.h"

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
    using mongo::ModifierRename;
    using mongo::Status;
    using mongo::StringData;
    using mongo::mutablebson::ConstElement;
    using mongo::mutablebson::Document;
    using mongo::mutablebson::Element;

    /** Helper to build and manipulate the mod. */
    class Mod {
    public:
        Mod() : _mod() {}

        explicit Mod(BSONObj modObj) {
            _modObj = modObj;
            ASSERT_OK(_mod.init(_modObj["$rename"].embeddedObject().firstElement()));
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

        ModifierRename& mod() { return _mod; }

    private:
        ModifierRename _mod;
        BSONObj _modObj;
    };

    /**
     * These test negative cases:
     *  -- No '$' support for positional operator
     *  -- Cannot move immutable field, or parts, of '_id' field
     *  -- No empty field names (ex. .a, b. )
     */
    TEST(InvalidInit, FromDbTests) {
        ModifierRename mod;
        ASSERT_NOT_OK(mod.init(fromjson("{'a.$':'b'}").firstElement()));
        ASSERT_NOT_OK(mod.init(fromjson("{'a':'b.$'}").firstElement()));
        ASSERT_NOT_OK(mod.init(fromjson("{'_id.a':'b'}").firstElement()));
        ASSERT_NOT_OK(mod.init(fromjson("{'b':'_id.a'}").firstElement()));
        ASSERT_NOT_OK(mod.init(fromjson("{'_id.a':'_id.b'}").firstElement()));
        ASSERT_NOT_OK(mod.init(fromjson("{'_id.b':'_id.a'}").firstElement()));
        ASSERT_NOT_OK(mod.init(fromjson("{'.b':'a'}").firstElement()));
        ASSERT_NOT_OK(mod.init(fromjson("{'b.':'a'}").firstElement()));
        ASSERT_NOT_OK(mod.init(fromjson("{'b':'.a'}").firstElement()));
        ASSERT_NOT_OK(mod.init(fromjson("{'b':'a.'}").firstElement()));
    }

    TEST(MissingFrom, InitPrepLog) {
        Document doc(fromjson("{a: 2}"));
        Mod setMod(fromjson("{$rename: {'b':'a'}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        BSONObj logObj = fromjson("{}");
        ASSERT_OK(setMod.log(&logBuilder));
        ASSERT_EQUALS(logDoc, logObj);
    }

    TEST(BasicInit, DifferentRoots) {
        Document doc(fromjson("{a: 2}"));
        Mod setMod(fromjson("{$rename: {'a':'f.g'}}"));
    }

    TEST(MoveOnSamePath, MoveUp) {
        ModifierRename mod;
        ASSERT_NOT_OK(mod.init(fromjson("{'b.a':'b'}").firstElement()));
    }

    TEST(MoveOnSamePath, MoveDown) {
        ModifierRename mod;
        ASSERT_NOT_OK(mod.init(fromjson("{'b':'b.a'}").firstElement()));
    }

    TEST(MissingTo, SimpleNumberAtRoot) {
        Document doc(fromjson("{a: 2}"));
        Mod setMod(fromjson("{$rename: {'a':'b'}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_EQUALS(execInfo.fieldRef[1]->dottedField(), "b");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(doc, fromjson("{b:2}"));

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        BSONObj logObj = fromjson("{$set:{ 'b': 2}, $unset: {'a': true}}");
        ASSERT_OK(setMod.log(&logBuilder));
        ASSERT_EQUALS(logDoc, logObj);
    }

    TEST(SimpleReplace, SameLevel) {
        Document doc(fromjson("{a: 2, b: 1}"));
        Mod setMod(fromjson("{$rename: {'a':'b'}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_EQUALS(execInfo.fieldRef[1]->dottedField(), "b");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(doc, fromjson("{b:2}"));

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        BSONObj logObj = fromjson("{$set:{ 'b': 2}, $unset: {'a': true}}");
        ASSERT_OK(setMod.log(&logBuilder));
        ASSERT_EQUALS(logDoc, logObj);
    }

    TEST(SimpleReplace, FromDottedElement) {
        Document doc(fromjson("{a: {c: {d: 6}}, b: 1}"));
        Mod setMod(fromjson("{$rename: {'a.c':'b'}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.c");
        ASSERT_EQUALS(execInfo.fieldRef[1]->dottedField(), "b");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(doc, fromjson("{a: {}, b:{ d: 6}}"));

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        BSONObj logObj = fromjson("{$set:{ 'b': {d: 6}}, $unset: {'a.c': true}}");
        ASSERT_OK(setMod.log(&logBuilder));
        ASSERT_EQUALS(logDoc, logObj);
    }

    TEST(DottedTo, MissingCompleteTo) {
        Document doc(fromjson("{a: 2, b: 1, c: {}}"));
        Mod setMod(fromjson("{$rename: {'a':'c.r.d'}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_EQUALS(execInfo.fieldRef[1]->dottedField(), "c.r.d");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(doc, fromjson("{b:1, c: { r: { d: 2}}}"));

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        BSONObj logObj = fromjson("{$set:{ 'c.r.d': 2}, $unset: {'a': true}}");
        ASSERT_OK(setMod.log(&logBuilder));
        ASSERT_EQUALS(logDoc, logObj);
    }

    TEST(DottedTo, ToIsCompletelyMissing) {
        Document doc(fromjson("{a: 2}"));
        Mod setMod(fromjson("{$rename: {'a':'b.c.d'}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_EQUALS(execInfo.fieldRef[1]->dottedField(), "b.c.d");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(doc, fromjson("{b: {c: {d: 2}}}"));

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        BSONObj logObj = fromjson("{$set:{ 'b.c.d': 2}, $unset: {'a': true}}");
        ASSERT_OK(setMod.log(&logBuilder));
        ASSERT_EQUALS(logDoc, logObj);
    }

    TEST(FromArrayOfEmbeddedDocs, ToMissingDottedField) {
        Document doc(fromjson("{a: [ {a:2, b:1} ] }"));
        Mod setMod(fromjson("{$rename: {'a':'b.c.d'}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_EQUALS(execInfo.fieldRef[1]->dottedField(), "b.c.d");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(setMod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(doc, fromjson("{b: {c: {d: [ {a:2, b:1} ]}}}"));

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        BSONObj logObj = fromjson("{$set:{ 'b.c.d': [ {a:2, b:1} ]}, $unset: {'a': true}}");
        ASSERT_OK(setMod.log(&logBuilder));
        ASSERT_EQUALS(logDoc, logObj);
    }

    TEST(FromArrayOfEmbeddedDocs, ToArray) {
        Document doc(fromjson("{a: [ {a:2, b:1} ] }"));
        Mod setMod(fromjson("{$rename: {'a.a':'a.b'}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_NOT_OK(setMod.prepare(doc.root(), "", &execInfo));
    }

    TEST(Arrays, MoveInto) {
        Document doc(fromjson("{a: [1, 2], b:2}"));
        Mod setMod(fromjson("{$rename: {'b':'a.2'}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_NOT_OK(setMod.prepare(doc.root(), "", &execInfo));
    }

    TEST(Arrays, MoveOut) {
        Document doc(fromjson("{a: [1, 2]}"));
        Mod setMod(fromjson("{$rename: {'a.0':'b'}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_NOT_OK(setMod.prepare(doc.root(), "", &execInfo));
    }

    TEST(Arrays, MoveNonexistantEmbeddedFieldOut) {
        Document doc(fromjson("{a: [{a:1}, {b:2}]}"));
        Mod setMod(fromjson("{$rename: {'a.a':'b'}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_NOT_OK(setMod.prepare(doc.root(), "", &execInfo));
    }

    TEST(Arrays, MoveEmbeddedFieldOutWithElementNumber) {
        Document doc(fromjson("{a: [{a:1}, {b:2}]}"));
        Mod setMod(fromjson("{$rename: {'a.0.a':'b'}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_NOT_OK(setMod.prepare(doc.root(), "", &execInfo));
    }

} // namespace
