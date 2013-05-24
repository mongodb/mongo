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


#include "mongo/db/ops/modifier_pop.h"

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
    using mongo::ModifierPop;
    using mongo::Status;
    using mongo::StringData;
    using mongo::mutablebson::checkDoc;
    using mongo::mutablebson::Document;
    using mongo::mutablebson::Element;

    /** Helper to build and manipulate a $pop mod. */
    class Mod {
    public:
        Mod() : _mod() {}

        explicit Mod(BSONObj modObj) {
            _modObj = modObj;
            ASSERT_OK(_mod.init(_modObj["$pop"].embeddedObject().firstElement()));
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
        ASSERT_OK(mod.init(modObj["$pop"].embeddedObject().firstElement()));
    }

    TEST(Init, BoolTrueArg) {
        BSONObj modObj = fromjson("{$pop: {a: true}}");
        ModifierPop mod;
        ASSERT_OK(mod.init(modObj["$pop"].embeddedObject().firstElement()));
    }

    TEST(Init, BoolFalseArg) {
        BSONObj modObj = fromjson("{$pop: {a: false}}");
        ModifierPop mod;
        ASSERT_OK(mod.init(modObj["$pop"].embeddedObject().firstElement()));
    }

    TEST(SimpleMod, PrepareBottom) {
        Document doc(fromjson("{a: [1,2]}"));
        Mod mod(fromjson("{$pop: {a: 1}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_FALSE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);
    }

    TEST(SimpleMod, PrepareApplyBottomO) {
        Document doc(fromjson("{a: [1,2]}"));
        Mod mod(fromjson("{$pop: {a: 0}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_FALSE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson(("{a: [1]}"))));
    }

    TEST(SimpleMod, PrepareTop) {
        Document doc(fromjson("{a: [1,2]}"));
        Mod mod(fromjson("{$pop: {a: -1}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_FALSE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);
    }

    TEST(SimpleMod, ApplyTopPop) {
        Document doc(fromjson("{a: [1,2]}"));
        Mod mod(fromjson("{$pop: {a: -1}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_FALSE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson(("{a: [2]}"))));
    }

    TEST(SimpleMod, ApplyBottomPop) {
        Document doc(fromjson("{a: [1,2]}"));
        Mod mod(fromjson("{$pop: {a: 1}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_FALSE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson(("{a: [1]}"))));
    }

    TEST(SimpleMod, ApplyLogBottomPop) {
        Document doc(fromjson("{a: [1,2]}"));
        Mod mod(fromjson("{$pop: {a: 1}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_FALSE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson(("{a:[1]}"))));

        Document logDoc;
        ASSERT_OK(mod.log(logDoc.root()));
        ASSERT_TRUE(checkDoc(logDoc, fromjson("{$set: {a: [1]}}")));
    }

    TEST(EmptyArray, PrepareNoOp) {
        Document doc(fromjson("{}"));
        Mod mod(fromjson("{$pop: {a: 1}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_TRUE(execInfo.inPlace);
        ASSERT_TRUE(execInfo.noOp);
    }

    TEST(EmptyArray, Log) {
        Document doc(fromjson("{a: []}"));
        Mod mod(fromjson("{$pop: {a: 1}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_TRUE(execInfo.inPlace);
        ASSERT_TRUE(execInfo.noOp);

        Document logDoc;
        ASSERT_OK(mod.log(logDoc.root()));
        ASSERT_TRUE(checkDoc(logDoc, fromjson("{$set: {a: []}}")));
    }

    TEST(SingleElemArray, ApplyLog) {
        Document doc(fromjson("{a: [1]}"));
        Mod mod(fromjson("{$pop: {a: 1}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_FALSE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson(("{a:[]}"))));

        Document logDoc;
        ASSERT_OK(mod.log(logDoc.root()));
        ASSERT_TRUE(checkDoc(logDoc, fromjson("{$set: {a: []}}")));
    }

    TEST(ArrayOfArray, ApplyLogPop) {
        Document doc(fromjson("{a: [[1,2], 1]}"));
        Mod mod(fromjson("{$pop: {'a.0': 1}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.0");
        ASSERT_FALSE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson(("{a:[[1], 1]}"))));

        Document logDoc;
        ASSERT_OK(mod.log(logDoc.root()));
        ASSERT_TRUE(checkDoc(logDoc, fromjson("{$set: { 'a.0': [1]}}")));
    }

    TEST(ArrayOfArray, ApplyLogPopOnlyElement) {
        Document doc(fromjson("{a: [[1], 1]}"));
        Mod mod(fromjson("{$pop: {'a.0': 1}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a.0");
        ASSERT_FALSE(execInfo.inPlace);
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod.apply());
        ASSERT_TRUE(checkDoc(doc, fromjson(("{a:[[], 1]}"))));

        Document logDoc;
        ASSERT_OK(mod.log(logDoc.root()));
        ASSERT_TRUE(checkDoc(logDoc, fromjson("{$set: { 'a.0': []}}")));
    }

    TEST(PrepareApplyLog, MissingPath) {
        Document doc(fromjson("{ a : [1, 2] }"));
        Mod mod(fromjson("{ $pop : { b : 1 } }"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_TRUE(execInfo.noOp);
        ASSERT_TRUE(execInfo.inPlace);

        Document logDoc;
        ASSERT_OK(mod.log(logDoc.root()));
        ASSERT_TRUE(checkDoc(logDoc, fromjson("{ $unset : { b : true } }")));
    }
} // unnamed namespace
