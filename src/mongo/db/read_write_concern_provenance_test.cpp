// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/read_write_concern_provenance.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

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

DEATH_TEST(ReadWriteConcernProvenanceTestDeathTest,
           SetSourceFromSomethingToUnset,
           "attempting to re-set provenance") {
    ReadWriteConcernProvenance provenance(ReadWriteConcernProvenance::Source::clientSupplied);
    provenance.setSource(boost::none);
}

DEATH_TEST(ReadWriteConcernProvenanceTestDeathTest,
           SetSourceFromSomethingToSomethingElse,
           "attempting to re-set provenance") {
    ReadWriteConcernProvenance provenance(ReadWriteConcernProvenance::Source::clientSupplied);
    provenance.setSource(ReadWriteConcernProvenance::Source::implicitDefault);
}

TEST(ReadWriteConcernProvenanceTest, ParseAbsentElement) {
    BSONObj obj = BSON("something" << "else");
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
    BSONObj obj = BSON("provenance" << "clientSupplied");
    auto provenance =
        ReadWriteConcernProvenance::parse(IDLParserContext("ReadWriteConcernProvenanceTest"), obj);
    ASSERT_TRUE(ReadWriteConcernProvenance::Source::clientSupplied == provenance.getSource());
}

TEST(ReadWriteConcernProvenanceTest, ParseInvalidSource) {
    BSONObj obj = BSON("provenance" << "foobar");
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
    ASSERT_BSONOBJ_EQ(BSON("provenance" << "clientSupplied"), builder.obj());
}

}  // namespace
}  // namespace mongo
