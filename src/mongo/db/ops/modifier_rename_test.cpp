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

#include "mongo/db/ops/modifier_rename.h"

#include <cstdint>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/mutable_bson_test_utils.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/update/log_builder.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

using mutablebson::ConstElement;
using mutablebson::Element;

/** Helper to build and manipulate the mod. */
class Mod {
public:
    Mod() : _mod() {}

    explicit Mod(BSONObj modObj) {
        _modObj = modObj;
        ASSERT_OK(_mod.init(_modObj["$rename"].embeddedObject().firstElement(),
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

    ModifierRename& mod() {
        return _mod;
    }

private:
    ModifierRename _mod;
    BSONObj _modObj;
};

/**
 * These test negative cases:
 *  -- No '$' support for positional operator
 *  -- No empty field names (ex. .a, b. )
 *  -- Can't rename to an invalid fieldname (empty fieldname part)
 */
TEST(InvalidInit, FromDbTests) {
    ModifierRename mod;
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_NOT_OK(mod.init(fromjson("{'a.$':'b'}").firstElement(),
                           ModifierInterface::Options::normal(expCtx)));
    ASSERT_NOT_OK(mod.init(fromjson("{'a':'b.$'}").firstElement(),
                           ModifierInterface::Options::normal(expCtx)));
    ASSERT_NOT_OK(mod.init(fromjson("{'.b':'a'}").firstElement(),
                           ModifierInterface::Options::normal(expCtx)));
    ASSERT_NOT_OK(mod.init(fromjson("{'b.':'a'}").firstElement(),
                           ModifierInterface::Options::normal(expCtx)));
    ASSERT_NOT_OK(mod.init(fromjson("{'b':'.a'}").firstElement(),
                           ModifierInterface::Options::normal(expCtx)));
    ASSERT_NOT_OK(mod.init(fromjson("{'b':'a.'}").firstElement(),
                           ModifierInterface::Options::normal(expCtx)));
}

TEST(InvalidInit, ToFieldCannotContainEmbeddedNullByte) {
    ModifierRename mod;
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    {
        const auto embeddedNull = "a\0b"_sd;
        ASSERT_NOT_OK(mod.init(BSON("a" << embeddedNull).firstElement(),
                               ModifierInterface::Options::normal(expCtx)));
    }

    {
        const auto singleNullByte = "\0"_sd;
        ASSERT_NOT_OK(mod.init(BSON("a" << singleNullByte).firstElement(),
                               ModifierInterface::Options::normal(expCtx)));
    }

    {
        const auto leadingNullByte = "\0bbbb"_sd;
        ASSERT_NOT_OK(mod.init(BSON("a" << leadingNullByte).firstElement(),
                               ModifierInterface::Options::normal(expCtx)));
    }

    {
        const auto trailingNullByte = "bbbb\0"_sd;
        ASSERT_NOT_OK(mod.init(BSON("a" << trailingNullByte).firstElement(),
                               ModifierInterface::Options::normal(expCtx)));
    }
}

TEST(MissingFrom, InitPrepLog) {
    mutablebson::Document doc(fromjson("{a: 2}"));
    Mod setMod(fromjson("{$rename: {'b':'a'}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

    mutablebson::Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    BSONObj logObj = fromjson("{}");
    ASSERT_OK(setMod.log(&logBuilder));
    ASSERT_EQUALS(logDoc, logObj);
}

TEST(MissingFromDotted, InitPrepLog) {
    mutablebson::Document doc(fromjson("{a: {r:2}}"));
    Mod setMod(fromjson("{$rename: {'a.b':'a.c'}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));
    ASSERT_TRUE(execInfo.noOp);

    mutablebson::Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    BSONObj logObj = fromjson("{}");
    ASSERT_OK(setMod.log(&logBuilder));
    ASSERT_EQUALS(logDoc, logObj);
}

TEST(BasicInit, DifferentRoots) {
    mutablebson::Document doc(fromjson("{a: 2}"));
    Mod setMod(fromjson("{$rename: {'a':'f.g'}}"));
}

TEST(MoveOnSamePath, MoveUp) {
    ModifierRename mod;
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_NOT_OK(mod.init(fromjson("{'b.a':'b'}").firstElement(),
                           ModifierInterface::Options::normal(expCtx)));
}

TEST(MoveOnSamePath, MoveDown) {
    ModifierRename mod;
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_NOT_OK(mod.init(fromjson("{'b':'b.a'}").firstElement(),
                           ModifierInterface::Options::normal(expCtx)));
}

TEST(MoveOnSamePath, MoveToSelf) {
    ModifierRename mod;
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_NOT_OK(mod.init(fromjson("{'b.a':'b.a'}").firstElement(),
                           ModifierInterface::Options::normal(expCtx)));
}

TEST(MissingTo, SimpleNumberAtRoot) {
    mutablebson::Document doc(fromjson("{a: 2}"));
    Mod setMod(fromjson("{$rename: {'a':'b'}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_EQUALS(execInfo.fieldRef[1]->dottedField(), "b");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(setMod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(doc, fromjson("{b:2}"));

    mutablebson::Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    BSONObj logObj = fromjson("{$set:{ 'b': 2}, $unset: {'a': true}}");
    ASSERT_OK(setMod.log(&logBuilder));
    ASSERT_EQUALS(logDoc, logObj);
}

TEST(SimpleReplace, SameLevel) {
    mutablebson::Document doc(fromjson("{a: 2, b: 1}"));
    Mod setMod(fromjson("{$rename: {'a':'b'}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_EQUALS(execInfo.fieldRef[1]->dottedField(), "b");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(setMod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(doc, fromjson("{b:2}"));

    mutablebson::Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    BSONObj logObj = fromjson("{$set:{ 'b': 2}, $unset: {'a': true}}");
    ASSERT_OK(setMod.log(&logBuilder));
    ASSERT_EQUALS(logDoc, logObj);
}

TEST(SimpleReplace, FromDottedElement) {
    mutablebson::Document doc(fromjson("{a: {c: {d: 6}}, b: 1}"));
    Mod setMod(fromjson("{$rename: {'a.c':'b'}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.c");
    ASSERT_EQUALS(execInfo.fieldRef[1]->dottedField(), "b");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(setMod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(doc, fromjson("{a: {}, b:{ d: 6}}"));

    mutablebson::Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    BSONObj logObj = fromjson("{$set:{ 'b': {d: 6}}, $unset: {'a.c': true}}");
    ASSERT_OK(setMod.log(&logBuilder));
    ASSERT_EQUALS(logDoc, logObj);
}

TEST(SimpleReplace, RenameToExistingFieldDoesNotReorderFields) {
    mutablebson::Document doc(fromjson("{a: 1, b: 2, c: 3}"));
    Mod setMod(fromjson("{$rename: {a: 'b'}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));
    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_EQUALS(execInfo.fieldRef[1]->dottedField(), "b");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(setMod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(doc, fromjson("{b: 1, c: 3}"));

    mutablebson::Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    BSONObj logObj = fromjson("{$set: {b: 1}, $unset: {a: true}}");
    ASSERT_OK(setMod.log(&logBuilder));
    ASSERT_EQUALS(logDoc, logObj);
}

TEST(SimpleReplace, RenameToExistingNestedFieldDoesNotReorderFields) {
    mutablebson::Document doc(fromjson("{a: {b: {c: 1, d: 2}}, b: 3, c: {d: 4}}"));
    Mod setMod(fromjson("{$rename: {'c.d': 'a.b.c'}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));
    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "c.d");
    ASSERT_EQUALS(execInfo.fieldRef[1]->dottedField(), "a.b.c");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(setMod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(doc, fromjson("{a: {b: {c: 4, d: 2}}, b: 3, c: {}}"));

    mutablebson::Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    BSONObj logObj = fromjson("{$set: {'a.b.c': 4}, $unset: {'c.d': true}}");
    ASSERT_OK(setMod.log(&logBuilder));
    ASSERT_EQUALS(logDoc, logObj);
}

TEST(DottedTo, MissingCompleteTo) {
    mutablebson::Document doc(fromjson("{a: 2, b: 1, c: {}}"));
    Mod setMod(fromjson("{$rename: {'a':'c.r.d'}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_EQUALS(execInfo.fieldRef[1]->dottedField(), "c.r.d");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(setMod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(doc, fromjson("{b:1, c: { r: { d: 2}}}"));

    mutablebson::Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    BSONObj logObj = fromjson("{$set:{ 'c.r.d': 2}, $unset: {'a': true}}");
    ASSERT_OK(setMod.log(&logBuilder));
    ASSERT_EQUALS(logDoc, logObj);
}

TEST(DottedTo, ToIsCompletelyMissing) {
    mutablebson::Document doc(fromjson("{a: 2}"));
    Mod setMod(fromjson("{$rename: {'a':'b.c.d'}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_EQUALS(execInfo.fieldRef[1]->dottedField(), "b.c.d");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(setMod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(doc, fromjson("{b: {c: {d: 2}}}"));

    mutablebson::Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    BSONObj logObj = fromjson("{$set:{ 'b.c.d': 2}, $unset: {'a': true}}");
    ASSERT_OK(setMod.log(&logBuilder));
    ASSERT_EQUALS(logDoc, logObj);
}

TEST(FromArrayOfEmbeddedDocs, ToMissingDottedField) {
    mutablebson::Document doc(fromjson("{a: [ {a:2, b:1} ] }"));
    Mod setMod(fromjson("{$rename: {'a':'b.c.d'}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_EQUALS(execInfo.fieldRef[1]->dottedField(), "b.c.d");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(setMod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(doc, fromjson("{b: {c: {d: [ {a:2, b:1} ]}}}"));

    mutablebson::Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    BSONObj logObj = fromjson("{$set:{ 'b.c.d': [ {a:2, b:1} ]}, $unset: {'a': true}}");
    ASSERT_OK(setMod.log(&logBuilder));
    ASSERT_EQUALS(logDoc, logObj);
}

TEST(FromArrayOfEmbeddedDocs, ToArray) {
    mutablebson::Document doc(fromjson("{a: [ {a:2, b:1} ] }"));
    Mod setMod(fromjson("{$rename: {'a.a':'a.b'}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_NOT_OK(setMod.prepare(doc.root(), "", &execInfo));
}

TEST(Arrays, MoveInto) {
    mutablebson::Document doc(fromjson("{a: [1, 2], b:2}"));
    Mod setMod(fromjson("{$rename: {'b':'a.2'}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_NOT_OK(setMod.prepare(doc.root(), "", &execInfo));
}

TEST(Arrays, MoveOut) {
    mutablebson::Document doc(fromjson("{a: [1, 2]}"));
    Mod setMod(fromjson("{$rename: {'a.0':'b'}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_NOT_OK(setMod.prepare(doc.root(), "", &execInfo));
}

TEST(Arrays, MoveNonexistantEmbeddedFieldOut) {
    mutablebson::Document doc(fromjson("{a: [{a:1}, {b:2}]}"));
    Mod setMod(fromjson("{$rename: {'a.a':'b'}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_NOT_OK(setMod.prepare(doc.root(), "", &execInfo));
}

TEST(Arrays, MoveEmbeddedFieldOutWithElementNumber) {
    mutablebson::Document doc(fromjson("{a: [{a:1}, {b:2}]}"));
    Mod setMod(fromjson("{$rename: {'a.0.a':'b'}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_NOT_OK(setMod.prepare(doc.root(), "", &execInfo));
}

TEST(Arrays, ReplaceArrayField) {
    mutablebson::Document doc(fromjson("{a: 2, b: []}"));
    Mod setMod(fromjson("{$rename: {'a':'b'}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_EQUALS(execInfo.fieldRef[1]->dottedField(), "b");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(setMod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(doc, fromjson("{b:2}"));

    mutablebson::Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    BSONObj logObj = fromjson("{$set:{ 'b': 2}, $unset: {'a': true}}");
    ASSERT_OK(setMod.log(&logBuilder));
    ASSERT_EQUALS(logDoc, logObj);
}


TEST(Arrays, ReplaceWithArrayField) {
    mutablebson::Document doc(fromjson("{a: [], b: 2}"));
    Mod setMod(fromjson("{$rename: {'a':'b'}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_EQUALS(execInfo.fieldRef[1]->dottedField(), "b");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(setMod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(doc, fromjson("{b:[]}"));

    mutablebson::Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    BSONObj logObj = fromjson("{$set:{ 'b': []}, $unset: {'a': true}}");
    ASSERT_OK(setMod.log(&logBuilder));
    ASSERT_EQUALS(logDoc, logObj);
}

TEST(LegacyData, CanRenameFromInvalidFieldName) {
    mutablebson::Document doc(fromjson("{$a: 2}"));
    Mod setMod(fromjson("{$rename: {'$a':'a'}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(setMod.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "$a");
    ASSERT_EQUALS(execInfo.fieldRef[1]->dottedField(), "a");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(setMod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(doc, fromjson("{a:2}"));

    mutablebson::Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    BSONObj logObj = fromjson("{$set:{ 'a': 2}, $unset: {'$a': true}}");
    ASSERT_OK(setMod.log(&logBuilder));
    ASSERT_EQUALS(logDoc, logObj);
}

}  // namespace mongo
