/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/update/v1_log_builder.h"

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/mutable/mutable_bson_test_utils.h"
#include "mongo/db/json.h"
#include "mongo/db/update/runtime_update_path.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/safe_num.h"

namespace mongo {
namespace {
namespace mmb = mongo::mutablebson;

/**
 * Given a FieldRef, creates a RuntimeUpdatePath based on it, assuming that every component is a
 * field name. This is safe to do while testing the V1 log builder, since it ignores the types of
 * the path given entirely.
 */
RuntimeUpdatePath makeRuntimeUpdatePathAssumeAllComponentsFieldNames(StringData path) {
    FieldRef fieldRef(path);
    return RuntimeUpdatePath(
        std::move(fieldRef),
        std::vector<RuntimeUpdatePath::ComponentType>(
            fieldRef.numParts(), RuntimeUpdatePath::ComponentType::kFieldName));
}

TEST(V1LogBuilder, UpdateFieldMutableBson) {
    mmb::Document doc;
    V1LogBuilder lb(doc.root());

    const mmb::Element elt_ab = doc.makeElementInt("a.b", 1);
    ASSERT_TRUE(elt_ab.ok());
    ASSERT_OK(
        lb.logUpdatedField(makeRuntimeUpdatePathAssumeAllComponentsFieldNames("a.b"), elt_ab));

    ASSERT_BSONOBJ_BINARY_EQ(mongo::fromjson("{ $set : { 'a.b' : 1 } }"), lb.serialize());
}

TEST(V1LogBuilder, CreateField) {
    mmb::Document doc;
    V1LogBuilder lb(doc.root());

    const mmb::Element elt_ab = doc.makeElementInt("a.b", 1);
    ASSERT_TRUE(elt_ab.ok());
    ASSERT_OK(lb.logCreatedField(makeRuntimeUpdatePathAssumeAllComponentsFieldNames("a.b"),
                                 0,  // idxOfFirstNewComponent (unused)
                                 elt_ab));

    ASSERT_BSONOBJ_BINARY_EQ(mongo::fromjson("{ $set : { 'a.b' : 1 } }"), lb.serialize());
}

TEST(V1LogBuilder, CreateFieldBSONElt) {
    mmb::Document doc;
    V1LogBuilder lb(doc.root());

    BSONObj storage = BSON("a" << 1);
    ASSERT_OK(lb.logCreatedField(makeRuntimeUpdatePathAssumeAllComponentsFieldNames("a.b"),
                                 0,  // idxOfFirstNewComponent (unused)
                                 storage.firstElement()));

    ASSERT_BSONOBJ_BINARY_EQ(mongo::fromjson("{ $set : { 'a.b' : 1 } }"), lb.serialize());
}

TEST(V1LogBuilder, AddOneToUnset) {
    mmb::Document doc;
    V1LogBuilder lb(doc.root());
    ASSERT_OK(lb.logDeletedField(makeRuntimeUpdatePathAssumeAllComponentsFieldNames("x.y")));
    ASSERT_EQUALS(mongo::fromjson("{ $unset : { 'x.y' : true } }"), doc);
}
TEST(V1LogBuilder, AddOneToEach) {
    mmb::Document doc;
    V1LogBuilder lb(doc.root());

    const mmb::Element elt_ab = doc.makeElementInt("", 1);
    ASSERT_TRUE(elt_ab.ok());
    ASSERT_OK(
        lb.logUpdatedField(makeRuntimeUpdatePathAssumeAllComponentsFieldNames("a.b"), elt_ab));

    const mmb::Element elt_cd = doc.makeElementInt("", 2);
    ASSERT_TRUE(elt_cd.ok());

    ASSERT_OK(lb.logCreatedField(makeRuntimeUpdatePathAssumeAllComponentsFieldNames("c.d"),
                                 0,  // idxOfCreatedComponent (unused)
                                 elt_cd));

    ASSERT_OK(lb.logDeletedField(makeRuntimeUpdatePathAssumeAllComponentsFieldNames("x.y")));

    ASSERT_EQUALS(mongo::fromjson("{ "
                                  "   $set : { 'a.b' : 1, 'c.d': 2 }, "
                                  "   $unset : { 'x.y' : true } "
                                  "}"),
                  doc);
}
TEST(V1LogBuilder, VerifySetsAreGrouped) {
    mmb::Document doc;
    V1LogBuilder lb(doc.root());

    const mmb::Element elt_ab = doc.makeElementInt("a.b", 1);
    ASSERT_TRUE(elt_ab.ok());
    ASSERT_OK(
        lb.logUpdatedField(makeRuntimeUpdatePathAssumeAllComponentsFieldNames("a.b"), elt_ab));

    const mmb::Element elt_xy = doc.makeElementInt("x.y", 1);
    ASSERT_TRUE(elt_xy.ok());
    ASSERT_OK(
        lb.logUpdatedField(makeRuntimeUpdatePathAssumeAllComponentsFieldNames("x.y"), elt_xy));

    ASSERT_EQUALS(mongo::fromjson("{ $set : {"
                                  "   'a.b' : 1, "
                                  "   'x.y' : 1 "
                                  "} }"),
                  doc);
}

TEST(V1LogBuilder, VerifyUnsetsAreGrouped) {
    mmb::Document doc;
    V1LogBuilder lb(doc.root());

    ASSERT_OK(lb.logDeletedField(makeRuntimeUpdatePathAssumeAllComponentsFieldNames("a.b")));
    ASSERT_OK(lb.logDeletedField(makeRuntimeUpdatePathAssumeAllComponentsFieldNames("x.y")));

    ASSERT_EQUALS(mongo::fromjson("{ $unset : {"
                                  "   'a.b' : true, "
                                  "   'x.y' : true "
                                  "} }"),
                  doc);
}
}  // namespace
}  // namespace mongo
