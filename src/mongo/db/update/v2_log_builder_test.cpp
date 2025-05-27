/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/update/v2_log_builder.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/mutable_bson/document.h"
#include "mongo/db/field_ref.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/safe_num.h"

#include <string>
#include <utility>
#include <vector>

namespace mongo::v2_log_builder {
namespace {
namespace mmb = mongo::mutablebson;

/**
 * Given a FieldRef, creates a RuntimeUpdatePath based on it, assuming that every numeric component
 * is an array index.
 */
RuntimeUpdatePath makeRuntimeUpdatePathAssumeNumericComponentsAreIndexes(StringData path) {
    FieldRef fr(path);
    std::vector<RuntimeUpdatePath::ComponentType> types;

    for (int i = 0; i < fr.numParts(); ++i) {
        types.push_back(fr.isNumericPathComponentStrict(i)
                            ? RuntimeUpdatePath::ComponentType::kArrayIndex
                            : RuntimeUpdatePath::ComponentType::kFieldName);
    }
    return RuntimeUpdatePath(fr, std::move(types));
}

/**
 * Given a FieldRef, creates a RuntimeUpdatePath based on it, assuming that every component is a
 * field name.
 */
RuntimeUpdatePath makeRuntimeUpdatePathAssumeAllComponentsFieldNames(std::string path) {
    FieldRef fieldRef(path);
    RuntimeUpdatePath::ComponentTypeVector types(fieldRef.numParts(),
                                                 RuntimeUpdatePath::ComponentType::kFieldName);
    return RuntimeUpdatePath(std::move(fieldRef), std::move(types));
}

TEST(V2LogBuilder, UpdateFieldWithTopLevelMutableBsonElement) {
    mmb::Document doc;
    // Element is an array. Modifying a sub-element of mmb::Element does not store the data
    // serialized. So we need to call Element::writeToArray() in those cases to serialize the array.
    // We explicity modify the sub-element so that this logic can be tested.
    const mmb::Element eltArr =
        doc.makeElementArray("", BSON_ARRAY(1 << BSON("sub" << "obj") << 3));
    ASSERT_OK(eltArr.leftChild().setValueString("val"));

    // Element is an obj. Modifying a sub-element of mmb::Element does not store the data
    // serialized. So we need to call Element::writeChildren() in those cases to serialize the
    // object. We explicity modify the sub-element so that this logic can be tested.
    mmb::Element eltObj = doc.makeElementObject("", fromjson("{arr: [{a: [0]}, 2]}"));
    ASSERT_OK(eltObj.leftChild().rightChild().setValueString("val"));

    // Element is a scalar.
    const mmb::Element eltScalar = doc.makeElementString("c.d", "val");
    ASSERT_TRUE(eltScalar.ok());

    V2LogBuilder lb;
    ASSERT_OK(
        lb.logUpdatedField(makeRuntimeUpdatePathAssumeAllComponentsFieldNames("a.b"), eltArr));
    ASSERT_OK(
        lb.logUpdatedField(makeRuntimeUpdatePathAssumeAllComponentsFieldNames("c.d"), eltObj));
    ASSERT_OK(
        lb.logUpdatedField(makeRuntimeUpdatePathAssumeAllComponentsFieldNames("c.e"), eltScalar));
    ASSERT_BSONOBJ_BINARY_EQ(
        mongo::fromjson("{ $v: 2, diff: { sa: {u: {b: ['val', {sub: 'obj'}, 3]}}, sc: {u: {d: "
                        "{arr: [{a: [0]}, 'val']}, e: 'val'}} }}"),
        lb.serialize());
}

TEST(V2LogBuilder, UpdateFieldMutableBson) {
    auto storage = fromjson("{subArray: [{a: [0]}], array: [{}], scalar: 'val'}");
    mmb::Document doc(
        fromjson("{topLevel: {obj: {subArray: 1}, array: [[], [0], {}], scalar: 'val'}}"));

    // Element is an obj. Modifying a sub-element of mmb::Element does not store the data
    // serialized. So we need to call Element::writeChildren() in those cases to serialize the
    // object. We explicity modify the sub-element so that this logic can be tested.
    mmb::Element eltObj = doc.root().leftChild().leftChild();
    ASSERT_OK(eltObj.leftChild().setValueBSONElement(storage["subArray"]));

    // Element is an array.
    const mmb::Element eltArr = eltObj.rightSibling();
    ASSERT_OK(eltArr.leftChild().setValueBSONElement(storage["array"]));

    // Element is a scalar.
    const mmb::Element eltScalar = eltArr.rightSibling();
    ASSERT_TRUE(eltScalar.ok());

    V2LogBuilder lb;
    ASSERT_OK(
        lb.logUpdatedField(makeRuntimeUpdatePathAssumeAllComponentsFieldNames("a.b"), eltArr));
    ASSERT_OK(
        lb.logUpdatedField(makeRuntimeUpdatePathAssumeAllComponentsFieldNames("c.d"), eltObj));
    ASSERT_OK(
        lb.logUpdatedField(makeRuntimeUpdatePathAssumeAllComponentsFieldNames("c.e"), eltScalar));
    ASSERT_BSONOBJ_BINARY_EQ(
        mongo::fromjson("{ $v: 2, diff: { sa: {u: {b: [[{}], [0], {}]}}, sc: {u: {d: "
                        "{subArray: [{a: [0]}]}, e: 'val'}} }}"),
        lb.serialize());
}

TEST(V2LogBuilder, CreateFieldBSONElt) {
    BSONObj storage = fromjson("{arr: ['v','a','l'], obj: {}, scalar: 2}");
    V2LogBuilder lb;
    ASSERT_OK(lb.logCreatedField(
        makeRuntimeUpdatePathAssumeAllComponentsFieldNames("a.b.d"), 0, storage["arr"]));
    ASSERT_OK(lb.logCreatedField(
        makeRuntimeUpdatePathAssumeAllComponentsFieldNames("a.c.e"), 0, storage["obj"]));
    ASSERT_OK(lb.logCreatedField(
        makeRuntimeUpdatePathAssumeAllComponentsFieldNames("a.b.c"), 0, storage["scalar"]));
    ASSERT_BSONOBJ_BINARY_EQ(
        mongo::fromjson("{ $v: 2, diff: {i : {a: {b: {d: ['v','a','l'], c: 2}, c: {e: {}}} }}}"),
        lb.serialize());
}

TEST(V2LogBuilder, CreateFieldMutableBson) {
    mmb::Document doc;
    V2LogBuilder lb;

    const mmb::Element elt_ab = doc.makeElementInt("a.b", 1);
    ASSERT_TRUE(elt_ab.ok());
    ASSERT_OK(lb.logCreatedField(makeRuntimeUpdatePathAssumeAllComponentsFieldNames("a.b"),
                                 0,  // idxOfFirstNewComponent (unused)
                                 elt_ab));
    ASSERT_BSONOBJ_BINARY_EQ(mongo::fromjson("{ $v: 2, diff: { i: {a: {b: 1}}}}"), lb.serialize());
}

TEST(V2LogBuilder, CreateFieldMergeInOrder) {
    mmb::Document doc;
    const mmb::Element eltScalar = doc.makeElementInt("scalar", 1);
    const mmb::Element eltObj = doc.makeElementObject("obj", fromjson("{arr: [{a: [0]}]}"));
    ASSERT_TRUE(eltObj.ok());
    ASSERT_TRUE(eltScalar.ok());

    // Test where only a 'root' object exists and logCreateField() needs to create sub-objects
    // recursively.
    const int idxOfFirstNewComponent = 1;
    V2LogBuilder lb;
    ASSERT_OK(lb.logCreatedField(makeRuntimeUpdatePathAssumeAllComponentsFieldNames("root.d"),
                                 idxOfFirstNewComponent,
                                 eltScalar));
    ASSERT_OK(lb.logCreatedField(makeRuntimeUpdatePathAssumeAllComponentsFieldNames("root.b.a"),
                                 idxOfFirstNewComponent,
                                 eltScalar));
    ASSERT_OK(lb.logCreatedField(makeRuntimeUpdatePathAssumeAllComponentsFieldNames("root.c.b"),
                                 idxOfFirstNewComponent,
                                 eltObj));
    ASSERT_OK(lb.logCreatedField(makeRuntimeUpdatePathAssumeAllComponentsFieldNames("root.c.e"),
                                 idxOfFirstNewComponent,
                                 eltScalar));
    ASSERT_OK(lb.logCreatedField(makeRuntimeUpdatePathAssumeAllComponentsFieldNames("root.a"),
                                 idxOfFirstNewComponent,
                                 eltObj));
    ASSERT_BSONOBJ_BINARY_EQ(mongo::fromjson("{ $v: 2, diff: { sroot: {i: {d: 1, b: {a: 1}, c: {b: "
                                             "{arr: [{a: [0]}]}, e: 1}, a: {arr: [{a: [0]}]}} }}}"),
                             lb.serialize());
}

TEST(V2LogBuilder, BasicDelete) {
    mmb::Document doc;
    V2LogBuilder lb;
    ASSERT_OK(lb.logDeletedField(makeRuntimeUpdatePathAssumeAllComponentsFieldNames("x")));
    ASSERT_BSONOBJ_BINARY_EQ(mongo::fromjson("{ $v: 2, diff: { d: {x: false}}}"), lb.serialize());
}

TEST(V2LogBuilder, VerifyDeletesOrder) {
    mmb::Document doc;
    V2LogBuilder lb;

    ASSERT_OK(lb.logDeletedField(makeRuntimeUpdatePathAssumeAllComponentsFieldNames("a.c")));
    ASSERT_OK(lb.logDeletedField(makeRuntimeUpdatePathAssumeAllComponentsFieldNames("a.b")));
    ASSERT_OK(lb.logDeletedField(makeRuntimeUpdatePathAssumeAllComponentsFieldNames("x.z")));
    ASSERT_OK(lb.logDeletedField(makeRuntimeUpdatePathAssumeAllComponentsFieldNames("x.y")));

    ASSERT_BSONOBJ_BINARY_EQ(
        mongo::fromjson(
            "{ $v: 2, diff: { sa: {d: {c: false, b: false}}, sx: {d: {z: false, y: false}} }}"),
        lb.serialize());
}

TEST(V2LogBuilder, WithAllModificationTypes) {
    mmb::Document doc;
    V2LogBuilder lb;

    const mmb::Element elt_ab = doc.makeElementInt("", 1);
    ASSERT_TRUE(elt_ab.ok());
    ASSERT_OK(
        lb.logUpdatedField(makeRuntimeUpdatePathAssumeAllComponentsFieldNames("a.b.c"), elt_ab));

    const mmb::Element elt_cd = doc.makeElementInt("", 2);
    ASSERT_TRUE(elt_cd.ok());

    ASSERT_OK(lb.logCreatedField(
        makeRuntimeUpdatePathAssumeAllComponentsFieldNames("a.b.d.e"), 3, elt_cd));

    ASSERT_OK(lb.logDeletedField(makeRuntimeUpdatePathAssumeAllComponentsFieldNames("a.b.e")));
    ASSERT_BSONOBJ_BINARY_EQ(
        mongo::fromjson("{ $v: 2, diff: {sa: {sb: {d: {e: false}, u: {c: 1}, sd: {i: {e: 2}} }}}}"),
        lb.serialize());
}

TEST(V2LogBuilder, VerifySubDiffsAreOrderedCorrectly) {
    mmb::Document doc;
    V2LogBuilder lb;

    const mmb::Element elt_ab = doc.makeElementInt("a.b", 1);
    ASSERT_TRUE(elt_ab.ok());
    ASSERT_OK(
        lb.logUpdatedField(makeRuntimeUpdatePathAssumeAllComponentsFieldNames("a.b.c"), elt_ab));
    ASSERT_OK(
        lb.logCreatedField(makeRuntimeUpdatePathAssumeAllComponentsFieldNames("a.c.d"), 2, elt_ab));

    const mmb::Element elt_xy = doc.makeElementInt("x.y", 2);
    ASSERT_TRUE(elt_xy.ok());
    ASSERT_OK(
        lb.logUpdatedField(makeRuntimeUpdatePathAssumeAllComponentsFieldNames("x.y.z"), elt_xy));
    ASSERT_OK(
        lb.logUpdatedField(makeRuntimeUpdatePathAssumeAllComponentsFieldNames("x.z.a"), elt_xy));

    ASSERT_BSONOBJ_BINARY_EQ(
        mongo::fromjson("{ $v: 2, diff: { sa: {sb: {u: {c: 1}}, sc: {i: {d: 1}}}, "
                        "sx: {sy: {u: {z: 2}}, sz: {u: {a: 2}}} }}"),
        lb.serialize());
}

TEST(V2LogBuilder, ArrayModifications) {
    mmb::Document doc;
    V2LogBuilder lb;

    const mmb::Element elt_ab = doc.makeElementInt("", 1);
    ASSERT_TRUE(elt_ab.ok());
    ASSERT_OK(lb.logUpdatedField(
        makeRuntimeUpdatePathAssumeNumericComponentsAreIndexes("a.0.c.update"), elt_ab));

    // Element is an obj.
    const mmb::Element eltObj = doc.makeElementObject("", fromjson("{arr: [{a: [0]}]}"));
    ASSERT_TRUE(eltObj.ok());

    const mmb::Element eltScalar = doc.makeElementInt("", 2);
    ASSERT_TRUE(eltScalar.ok());

    // The order of the update of array elements need not be maintained by the caller.
    ASSERT_OK(lb.logCreatedField(
        makeRuntimeUpdatePathAssumeNumericComponentsAreIndexes("a.10.insert.a"), 2, eltScalar));
    ASSERT_OK(
        lb.logUpdatedField(makeRuntimeUpdatePathAssumeNumericComponentsAreIndexes("a.3"), elt_ab));
    ASSERT_OK(lb.logCreatedField(
        makeRuntimeUpdatePathAssumeNumericComponentsAreIndexes("a.0.c.insert.a"), 3, eltScalar));
    ASSERT_OK(lb.logCreatedField(
        makeRuntimeUpdatePathAssumeNumericComponentsAreIndexes("a.0.c.insert.f"), 3, eltScalar));
    ASSERT_OK(lb.logCreatedField(
        makeRuntimeUpdatePathAssumeNumericComponentsAreIndexes("a.1"), 1, eltObj));
    ASSERT_OK(
        lb.logDeletedField(makeRuntimeUpdatePathAssumeNumericComponentsAreIndexes("a.0.c.del")));

    ASSERT_BSONOBJ_BINARY_EQ(
        mongo::fromjson("{$v: 2, diff: {sa: {a: true, s0: {sc: {d: {del: false}, u: {update: 1}, "
                        "i: {insert: {a: 2, f: 2}}}}, u1: {arr: [{a: [0]}]}, u3: 1, s10: {i: "
                        "{insert: {a: 2}}} }}}"),
        lb.serialize());
}

TEST(V2LogBuilder, ImplicityArrayElementCreationAllowed) {
    mmb::Document doc;
    V2LogBuilder lb;

    const mmb::Element elt = doc.makeElementInt("", 1);
    ASSERT_TRUE(elt.ok());
    ASSERT_OK(lb.logCreatedField(
        makeRuntimeUpdatePathAssumeNumericComponentsAreIndexes("a.10.b.a"), 1, elt));
    ASSERT_OK(lb.logCreatedField(
        makeRuntimeUpdatePathAssumeNumericComponentsAreIndexes("a.2.b.a"), 1, elt));
    ASSERT_OK(
        lb.logUpdatedField(makeRuntimeUpdatePathAssumeNumericComponentsAreIndexes("a.0.b.a"), elt));

    ASSERT_BSONOBJ_BINARY_EQ(
        mongo::fromjson("{$v: 2, diff: {sa: {a: true, s0: {sb: {u: {a: 1}}}, u2: "
                        "{b: {a: 1}}, u10: {b: {a: 1}} }}}"),
        lb.serialize());
}

DEATH_TEST(V2LogBuilder, ImplicityArrayCreationDisallowed, "invariant") {
    mmb::Document doc;
    V2LogBuilder lb;

    const mmb::Element elt = doc.makeElementInt("", 1);
    ASSERT_TRUE(elt.ok());
    auto status = lb.logCreatedField(
        makeRuntimeUpdatePathAssumeNumericComponentsAreIndexes("a.10.insert.a"), 0, elt);
}

}  // namespace
}  // namespace mongo::v2_log_builder
