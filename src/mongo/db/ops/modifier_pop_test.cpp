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


#include "mongo/db/ops/modifier_pop.h"

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/mutable_bson_test_utils.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/ops/log_builder.h"
#include "mongo/db/json.h"
#include "mongo/platform/cstdint.h"
#include "mongo/unittest/unittest.h"

namespace {

    using mongo::Array;
    using mongo::BSONObj;
    using mongo::LogBuilder;
    using mongo::fromjson;
    using mongo::ModifierInterface;
    using mongo::ModifierPop;
    using mongo::Status;
    using mongo::StringData;
    using mongo::mutablebson::Document;
    using mongo::mutablebson::Element;

    /** Helper to build and manipulate a $pop mod. */
    class Mod {
    public:
        Mod() : _mod() {}

        explicit Mod(BSONObj modObj) {
            _modObj = modObj;
            ASSERT_OK(_mod.init(_modObj["$pop"].embeddedObject().firstElement(),
                                ModifierInterface::Options::normal()));
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

        ModifierPop& mod() { return _mod; }

        BSONObj modObj() { return _modObj; }

    private:
        ModifierPop _mod;
        BSONObj _modObj;
    };

    //
    // Test init values which aren't numbers.
    // These are going to cause a pop from the bottom.
    //
    TEST(Init, StringArg) {
        BSONObj modObj = fromjson("{$pop: {a: 'hi'}}");
        ModifierPop mod;
        ASSERT_OK(mod.init(modObj["$pop"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal()));
    }

    TEST(Init, BoolTrueArg) {
        BSONObj modObj = fromjson("{$pop: {a: true}}");
        ModifierPop mod;
        ASSERT_OK(mod.init(modObj["$pop"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal()));
    }

    TEST(Init, BoolFalseArg) {
        BSONObj modObj = fromjson("{$pop: {a: false}}");
        ModifierPop mod;
        ASSERT_OK(mod.init(modObj["$pop"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal()));
    }

    TEST(MissingField, AllButApply) {
        Document doc(fromjson("{a: [1,2]}"));
        Mod mod(fromjson("{$pop: {s: 1}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "s");
        ASSERT_TRUE(execInfo.noOp);

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(fromjson("{$unset: {'s': true}}"), logDoc);
    }

    TEST(SimpleMod, PrepareBottom) {
        Document doc(fromjson("{a: [1,2]}"));
        Mod mod(fromjson("{$pop: {a: 1}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_FALSE(execInfo.noOp);
    }

    TEST(SimpleMod, PrepareApplyBottomO) {
        Document doc(fromjson("{a: [1,2]}"));
        Mod mod(fromjson("{$pop: {a: 0}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson(("{a: [1]}")), doc);
    }

    TEST(SimpleMod, PrepareTop) {
        Document doc(fromjson("{a: [1,2]}"));
        Mod mod(fromjson("{$pop: {a: -1}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_FALSE(execInfo.noOp);
    }

    TEST(SimpleMod, ApplyTopPop) {
        Document doc(fromjson("{a: [1,2]}"));
        Mod mod(fromjson("{$pop: {a: -1}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson(("{a: [2]}")), doc);
    }

    TEST(SimpleMod, ApplyBottomPop) {
        Document doc(fromjson("{a: [1,2]}"));
        Mod mod(fromjson("{$pop: {a: 1}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson(("{a: [1]}")), doc);
    }

    TEST(SimpleMod, ApplyLogBottomPop) {
        Document doc(fromjson("{a: [1,2]}"));
        Mod mod(fromjson("{$pop: {a: 1}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson(("{a:[1]}")), doc);

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(fromjson("{$set: {a: [1]}}"), logDoc);
    }

    TEST(EmptyArray, PrepareNoOp) {
        Document doc(fromjson("{}"));
        Mod mod(fromjson("{$pop: {a: 1}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_TRUE(execInfo.noOp);
    }

    TEST(SingleElemArray, ApplyLog) {
        Document doc(fromjson("{a: [1]}"));
        Mod mod(fromjson("{$pop: {a: 1}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson(("{a:[]}")), doc);

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(fromjson("{$set: {a: []}}"), logDoc);
    }

    TEST(ArrayOfArray, ApplyLogPop) {
        Document doc(fromjson("{a: [[1,2], 1]}"));
        Mod mod(fromjson("{$pop: {'a.0': 1}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.0");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson(("{a:[[1], 1]}")), doc);

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(fromjson("{$set: { 'a.0': [1]}}"), logDoc);
    }

    TEST(ArrayOfArray, ApplyLogPopOnlyElement) {
        Document doc(fromjson("{a: [[1], 1]}"));
        Mod mod(fromjson("{$pop: {'a.0': 1}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.0");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());
        ASSERT_EQUALS(fromjson(("{a:[[], 1]}")), doc);

        Document logDoc;
        LogBuilder logBuilder(logDoc.root());
        ASSERT_OK(mod.log(&logBuilder));
        ASSERT_EQUALS(fromjson("{$set: { 'a.0': []}}"), logDoc);
    }

    TEST(Prepare, MissingPath) {
        Document doc(fromjson("{ a : [1, 2] }"));
        Mod mod(fromjson("{ $pop : { b : 1 } }"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_TRUE(execInfo.noOp);
    }
} // unnamed namespace
