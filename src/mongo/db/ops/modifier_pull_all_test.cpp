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


#include "mongo/db/ops/modifier_pull_all.h"

#include <cstdint>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/mutable/algorithm.h"
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

    explicit Mod(BSONObj modObj,
                 ModifierInterface::Options options = ModifierInterface::Options::normal())
        : _modObj(modObj), _mod() {
        ASSERT_OK(_mod.init(_modObj["$pullAll"].embeddedObject().firstElement(), options));
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

    ModifierPullAll& mod() {
        return _mod;
    }

private:
    BSONObj _modObj;
    ModifierPullAll _mod;
};

TEST(Init, BadThings) {
    BSONObj modObj;
    ModifierPullAll mod;

    modObj = fromjson("{$pullAll: {a:1}}");
    ASSERT_NOT_OK(mod.init(modObj["$pullAll"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal()));

    modObj = fromjson("{$pullAll: {a:'test'}}");
    ASSERT_NOT_OK(mod.init(modObj["$pullAll"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal()));

    modObj = fromjson("{$pullAll: {a:{}}}");
    ASSERT_NOT_OK(mod.init(modObj["$pullAll"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal()));

    modObj = fromjson("{$pullAll: {a:true}}");
    ASSERT_NOT_OK(mod.init(modObj["$pullAll"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal()));
}

TEST(PrepareApply, SimpleNumber) {
    Document doc(fromjson("{ a : [1, 'a', {r:1, b:2}] }"));
    Mod mod(fromjson("{ $pullAll : { a : [1] } }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(mod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{ a : ['a', {r:1, b:2}] }"), doc);
}

TEST(PrepareApply, MissingElement) {
    Document doc(fromjson("{ a : [1, 'a', {r:1, b:2}] }"));
    Mod mod(fromjson("{ $pullAll : { a : ['r'] } }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_TRUE(execInfo.noOp);

    ASSERT_OK(mod.apply());
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{ a : [1, 'a', {r:1, b:2}] }"), doc);
}

TEST(PrepareApply, TwoElements) {
    Document doc(fromjson("{ a : [1, 'a', {r:1, b:2}] }"));
    Mod mod(fromjson("{ $pullAll : { a : [1, 'a'] } }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(mod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{ a : [{r:1, b:2}] }"), doc);
}

TEST(EmptyResult, RemoveEverythingOutOfOrder) {
    Document doc(fromjson("{ a : [1, 'a', {r:1, b:2}] }"));
    Mod mod(fromjson("{ $pullAll : {a : [ {r:1, b:2}, 1, 'a' ] }}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(mod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{ a : [] }"), doc);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(mod.log(&logBuilder));
    ASSERT_EQUALS(fromjson("{ $set : { a : [] } }"), logDoc);
}

TEST(EmptyResult, RemoveEverythingInOrder) {
    Document doc(fromjson("{ a : [1, 'a', {r:1, b:2}] }"));
    Mod mod(fromjson("{ $pullAll : { a : [1, 'a', {r:1, b:2} ] } }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(mod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{ a : [] }"), doc);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(mod.log(&logBuilder));
    ASSERT_EQUALS(fromjson("{ $set : { a : [] } }"), logDoc);
}

TEST(EmptyResult, RemoveEverythingAndThenSome) {
    Document doc(fromjson("{ a : [1, 'a', {r:1, b:2}] }"));
    Mod mod(fromjson("{ $pullAll : { a : [2,3,1,'r', {r:1, b:2}, 'a' ] } }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(mod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{ a : [] }"), doc);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(mod.log(&logBuilder));
    ASSERT_EQUALS(fromjson("{ $set : { a : [] } }"), logDoc);
}

TEST(PrepareLog, MissingPullValue) {
    Document doc(fromjson("{ a : [1, 'a', {r:1, b:2}] }"));
    Mod mod(fromjson("{ $pullAll : { a : [2] } }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_TRUE(execInfo.noOp);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(mod.log(&logBuilder));
    ASSERT_EQUALS(fromjson("{ $set : { a : [1, 'a', {r:1, b:2}] } }"), logDoc);
}

TEST(PrepareLog, MissingPath) {
    Document doc(fromjson("{ a : [1, 'a', {r:1, b:2}] }"));
    Mod mod(fromjson("{ $pullAll : { b : [1] } }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_TRUE(execInfo.noOp);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(mod.log(&logBuilder));
    ASSERT_EQUALS(fromjson("{ $unset : { b : true } }"), logDoc);
}

TEST(Prepare, MissingArrayElementPath) {
    Document doc(fromjson("{ a : [1, 2] }"));
    Mod mod(fromjson("{ $pullAll : { 'a.2' : [1] } }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_TRUE(execInfo.noOp);
}

TEST(Prepare, FromArrayElementPath) {
    Document doc(fromjson("{ a : [1, 2] }"));
    Mod mod(fromjson("{ $pullAll : { 'a.0' : [1] } }"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_NOT_OK(mod.prepare(doc.root(), "", &execInfo));
}


TEST(Collation, RespectsCollationFromOptions) {
    Document doc(fromjson("{ a : ['foo', 'bar' ] }"));
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kToLowerString);
    Mod mod(fromjson("{ $pullAll : { 'a' : ['FOO', 'BAR'] } }"),
            ModifierInterface::Options::normal(&collator));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(mod.apply());
    ASSERT_EQUALS(doc, fromjson("{ a : [] }"));
}

TEST(Collation, RespectsCollationFromSetCollation) {
    Document doc(fromjson("{ a : ['foo', 'bar' ] }"));
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kToLowerString);
    Mod mod(fromjson("{ $pullAll : { 'a' : ['FOO', 'BAR'] } }"));
    mod.mod().setCollator(&collator);

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(mod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(mod.apply());
    ASSERT_EQUALS(doc, fromjson("{ a : [] }"));
}
}  // namespace
