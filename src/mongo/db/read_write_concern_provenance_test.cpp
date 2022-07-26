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

#include "mongo/platform/basic.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/read_write_concern_provenance.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(ReadWriteConcernProvenanceTest, DefaultUnset) {
    ReadWriteConcernProvenance provenance;
    ASSERT_FALSE(provenance.hasSource());
    ASSERT_TRUE(provenance.isClientSupplied());
}

TEST(ReadWriteConcernProvenanceTest, ClientSupplied) {
    ReadWriteConcernProvenance provenance(ReadWriteConcernProvenance::Source::clientSupplied);
    ASSERT_TRUE(provenance.hasSource());
    ASSERT_TRUE(provenance.isClientSupplied());
}

TEST(ReadWriteConcernProvenanceTest, ImplicitDefault) {
    ReadWriteConcernProvenance provenance(ReadWriteConcernProvenance::Source::implicitDefault);
    ASSERT_TRUE(provenance.hasSource());
    ASSERT_FALSE(provenance.isClientSupplied());
}

TEST(ReadWriteConcernProvenanceTest, CustomDefault) {
    ReadWriteConcernProvenance provenance(ReadWriteConcernProvenance::Source::customDefault);
    ASSERT_TRUE(provenance.hasSource());
    ASSERT_FALSE(provenance.isClientSupplied());
}

TEST(ReadWriteConcernProvenanceTest, GetLastErrorDefaults) {
    ReadWriteConcernProvenance provenance(ReadWriteConcernProvenance::Source::getLastErrorDefaults);
    ASSERT_TRUE(provenance.hasSource());
    ASSERT_FALSE(provenance.isClientSupplied());
}

TEST(ReadWriteConcernProvenanceTest, InternalWriteDefault) {
    ReadWriteConcernProvenance provenance(ReadWriteConcernProvenance::Source::internalWriteDefault);
    ASSERT_TRUE(provenance.hasSource());
    ASSERT_FALSE(provenance.isClientSupplied());
}

TEST(ReadWriteConcernProvenanceTest, SetSourceFromUnsetToUnset) {
    ReadWriteConcernProvenance provenance;
    provenance.setSource(boost::none);
    ASSERT_FALSE(provenance.hasSource());
}

TEST(ReadWriteConcernProvenanceTest, SetSourceFromUnsetToSomething) {
    ReadWriteConcernProvenance provenance;
    provenance.setSource(ReadWriteConcernProvenance::Source::clientSupplied);
    ASSERT_TRUE(ReadWriteConcernProvenance::Source::clientSupplied == provenance.getSource());
}

TEST(ReadWriteConcernProvenanceTest, SetSourceFromSomethingToSame) {
    ReadWriteConcernProvenance provenance(ReadWriteConcernProvenance::Source::clientSupplied);
    provenance.setSource(ReadWriteConcernProvenance::Source::clientSupplied);
    ASSERT_TRUE(ReadWriteConcernProvenance::Source::clientSupplied == provenance.getSource());
}

DEATH_TEST(ReadWriteConcernProvenanceTest,
           SetSourceFromSomethingToUnset,
           "attempting to re-set provenance") {
    ReadWriteConcernProvenance provenance(ReadWriteConcernProvenance::Source::clientSupplied);
    provenance.setSource(boost::none);
}

DEATH_TEST(ReadWriteConcernProvenanceTest,
           SetSourceFromSomethingToSomethingElse,
           "attempting to re-set provenance") {
    ReadWriteConcernProvenance provenance(ReadWriteConcernProvenance::Source::clientSupplied);
    provenance.setSource(ReadWriteConcernProvenance::Source::implicitDefault);
}

TEST(ReadWriteConcernProvenanceTest, ParseAbsentElement) {
    BSONObj obj = BSON("something"
                       << "else");
    auto provenance =
        ReadWriteConcernProvenance::parse(IDLParserContext("ReadWriteConcernProvenanceTest"), obj);
    ASSERT_FALSE(provenance.hasSource());
}

TEST(ReadWriteConcernProvenanceTest, ParseNonString) {
    BSONObj obj = BSON("provenance" << 42);
    ASSERT_THROWS_CODE(
        ReadWriteConcernProvenance::parse(IDLParserContext("ReadWriteConcernProvenanceTest"), obj),
        DBException,
        ErrorCodes::TypeMismatch);
}

TEST(ReadWriteConcernProvenanceTest, ParseValidSource) {
    BSONObj obj = BSON("provenance"
                       << "clientSupplied");
    auto provenance =
        ReadWriteConcernProvenance::parse(IDLParserContext("ReadWriteConcernProvenanceTest"), obj);
    ASSERT_TRUE(ReadWriteConcernProvenance::Source::clientSupplied == provenance.getSource());
}

TEST(ReadWriteConcernProvenanceTest, ParseInvalidSource) {
    BSONObj obj = BSON("provenance"
                       << "foobar");
    ASSERT_THROWS_CODE(
        ReadWriteConcernProvenance::parse(IDLParserContext("ReadWriteConcernProvenanceTest"), obj),
        DBException,
        ErrorCodes::BadValue);
}

TEST(ReadWriteConcernProvenanceTest, SerializeUnset) {
    ReadWriteConcernProvenance provenance;
    BSONObjBuilder builder;
    provenance.serialize(&builder);
    ASSERT_BSONOBJ_EQ(BSONObj(), builder.obj());
}

TEST(ReadWriteConcernProvenanceTest, SerializeSet) {
    ReadWriteConcernProvenance provenance(ReadWriteConcernProvenance::Source::clientSupplied);
    BSONObjBuilder builder;
    provenance.serialize(&builder);
    ASSERT_BSONOBJ_EQ(BSON("provenance"
                           << "clientSupplied"),
                      builder.obj());
}

}  // namespace
}  // namespace mongo
