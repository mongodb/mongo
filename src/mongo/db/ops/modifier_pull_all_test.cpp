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


#include "mongo/db/ops/modifier_pull_all.h"

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
    using mongo::ModifierPullAll;
    using mongo::ModifierInterface;
    using mongo::NumberInt;
    using mongo::Status;
    using mongo::StringData;
    using mongo::fromjson;
    using mongo::mutablebson::ConstElement;
    using mongo::mutablebson::Document;
    using mongo::mutablebson::Element;

    /** Helper to build and manipulate the mod. */
    class Mod {
    public:
        Mod() : _mod() {}

        explicit Mod(BSONObj modObj)
            : _modObj(modObj)
            , _mod() {
            ASSERT_OK(_mod.init(_modObj["$pullAll"].embeddedObject().firstElement()));
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

        ModifierPullAll& mod() { return _mod; }

    private:
        BSONObj _modObj;
        ModifierPullAll _mod;
    };

    TEST(Init, BadThings) {
        BSONObj modObj;
        ModifierPullAll mod;

        modObj = fromjson("{$pullAll: {a:1}}");
        ASSERT_NOT_OK(mod.init(modObj["$pullAll"].embeddedObject().firstElement()));

        modObj = fromjson("{$pullAll: {a:'test'}}");
        ASSERT_NOT_OK(mod.init(modObj["$pullAll"].embeddedObject().firstElement()));

        modObj = fromjson("{$pullAll: {a:{}}}");
        ASSERT_NOT_OK(mod.init(modObj["$pullAll"].embeddedObject().firstElement()));

        modObj = fromjson("{$pullAll: {a:true}}");
        ASSERT_NOT_OK(mod.init(modObj["$pullAll"].embeddedObject().firstElement()));

    }

    TEST(PrepareApply, SimpleNumber) {
        Document doc(fromjson("{ a : [1, 'a', {r:1, b:2}] }"));
        Mod mod(fromjson("{ $pullAll : { a : [1] } }"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.noOp);
        ASSERT_FALSE(execInfo.inPlace);

        ASSERT_OK(mod.apply());
        ASSERT_EQUALS(fromjson("{ a : ['a', {r:1, b:2}] }"), doc);
    }

    TEST(PrepareApply, MissingElement) {
        Document doc(fromjson("{ a : [1, 'a', {r:1, b:2}] }"));
        Mod mod(fromjson("{ $pullAll : { a : ['r'] } }"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_TRUE(execInfo.noOp);
        ASSERT_TRUE(execInfo.inPlace);

        ASSERT_OK(mod.apply());
        ASSERT_EQUALS(fromjson("{ a : [1, 'a', {r:1, b:2}] }"), doc);
    }

    TEST(PrepareApply, TwoElements) {
        Document doc(fromjson("{ a : [1, 'a', {r:1, b:2}] }"));
        Mod mod(fromjson("{ $pullAll : { a : [1, 'a'] } }"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.noOp);
        ASSERT_FALSE(execInfo.inPlace);

        ASSERT_OK(mod.apply());
        ASSERT_EQUALS(fromjson("{ a : [{r:1, b:2}] }"), doc);
    }

    TEST(EmptyResult, RemoveEverythingOutOfOrder) {
        Document doc(fromjson("{ a : [1, 'a', {r:1, b:2}] }"));
        Mod mod(fromjson("{ $pullAll : {a : [ {r:1, b:2}, 1, 'a' ] }}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.noOp);
        ASSERT_FALSE(execInfo.inPlace);

        ASSERT_OK(mod.apply());
        ASSERT_EQUALS(fromjson("{ a : [] }"), doc);

        Document logDoc;
        ASSERT_OK(mod.log(logDoc.root()));
        ASSERT_EQUALS(fromjson("{ $set : { a : [] } }"), logDoc);
    }

    TEST(EmptyResult, RemoveEverythingInOrder) {
        Document doc(fromjson("{ a : [1, 'a', {r:1, b:2}] }"));
        Mod mod(fromjson("{ $pullAll : { a : [1, 'a', {r:1, b:2} ] } }"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.noOp);
        ASSERT_FALSE(execInfo.inPlace);

        ASSERT_OK(mod.apply());
        ASSERT_EQUALS(fromjson("{ a : [] }"), doc);

        Document logDoc;
        ASSERT_OK(mod.log(logDoc.root()));
        ASSERT_EQUALS(fromjson("{ $set : { a : [] } }"), logDoc);
    }

    TEST(EmptyResult, RemoveEverythingAndThenSome) {
        Document doc(fromjson("{ a : [1, 'a', {r:1, b:2}] }"));
        Mod mod(fromjson("{ $pullAll : { a : [2,3,1,'r', {r:1, b:2}, 'a' ] } }"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_FALSE(execInfo.noOp);
        ASSERT_FALSE(execInfo.inPlace);

        ASSERT_OK(mod.apply());
        ASSERT_EQUALS(fromjson("{ a : [] }"), doc);

        Document logDoc;
        ASSERT_OK(mod.log(logDoc.root()));
        ASSERT_EQUALS(fromjson("{ $set : { a : [] } }"), logDoc);
    }

    TEST(PrepareLog, MissingPullValue) {
        Document doc(fromjson("{ a : [1, 'a', {r:1, b:2}] }"));
        Mod mod(fromjson("{ $pullAll : { a : [2] } }"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_TRUE(execInfo.noOp);
        ASSERT_TRUE(execInfo.inPlace);

        Document logDoc;
        ASSERT_OK(mod.log(logDoc.root()));
        ASSERT_EQUALS(fromjson("{ $set : { a : [1, 'a', {r:1, b:2}] } }"), logDoc);
    }

    TEST(PrepareLog, MissingPath) {
        Document doc(fromjson("{ a : [1, 'a', {r:1, b:2}] }"));
        Mod mod(fromjson("{ $pullAll : { b : [1] } }"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
        ASSERT_TRUE(execInfo.noOp);
        ASSERT_TRUE(execInfo.inPlace);

        Document logDoc;
        ASSERT_OK(mod.log(logDoc.root()));
        ASSERT_EQUALS(fromjson("{ $unset : { b : true } }"), logDoc);
    }

} // namespace
