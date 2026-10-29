// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/global_catalog/type_mongos.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"

namespace {

using namespace mongo;

TEST(Validity, MissingName) {
    BSONObj obj = BSON(MongosType::ping(Date_t::fromMillisSinceEpoch(1))
                       << MongosType::created(Date_t::fromMillisSinceEpoch(1))
                       << MongosType::uptime(100) << MongosType::waiting(false)
                       << MongosType::mongoVersion("x.x.x") << MongosType::configVersion(0)
                       << MongosType::advisoryHostFQDNs(BSONArrayBuilder().arr()));

    auto mongosTypeResult = MongosType::fromBSON(obj);
    ASSERT_EQ(ErrorCodes::NoSuchKey, mongosTypeResult.getStatus());
}

TEST(Validity, MissingCreated) {
    BSONObj obj = BSON(MongosType::name("localhost:27017")
                       << MongosType::ping(Date_t::fromMillisSinceEpoch(1))
                       << MongosType::uptime(100) << MongosType::waiting(false)
                       << MongosType::mongoVersion("x.x.x") << MongosType::configVersion(0)
                       << MongosType::advisoryHostFQDNs(BSONArrayBuilder().arr()));

    auto mongosTypeResult = MongosType::fromBSON(obj);
    ASSERT_OK(mongosTypeResult.getStatus());
    MongosType& mtype = mongosTypeResult.getValue();
    ASSERT_OK(mtype.validate());
}

TEST(Validity, MissingPing) {
    BSONObj obj = BSON(MongosType::name("localhost:27017")
                       << MongosType::created(Date_t::fromMillisSinceEpoch(1))
                       << MongosType::uptime(100) << MongosType::waiting(false)
                       << MongosType::mongoVersion("x.x.x") << MongosType::configVersion(0)
                       << MongosType::advisoryHostFQDNs(BSONArrayBuilder().arr()));

    auto mongosTypeResult = MongosType::fromBSON(obj);
    ASSERT_EQ(ErrorCodes::NoSuchKey, mongosTypeResult.getStatus());
}

TEST(Validity, MissingUp) {
    BSONObj obj =
        BSON(MongosType::name("localhost:27017")
             << MongosType::created(Date_t::fromMillisSinceEpoch(1))
             << MongosType::ping(Date_t::fromMillisSinceEpoch(1)) << MongosType::waiting(false)
             << MongosType::mongoVersion("x.x.x") << MongosType::configVersion(0)
             << MongosType::advisoryHostFQDNs(BSONArrayBuilder().arr()));

    auto mongosTypeResult = MongosType::fromBSON(obj);
    ASSERT_EQ(ErrorCodes::NoSuchKey, mongosTypeResult.getStatus());
}

TEST(Validity, MissingWaiting) {
    BSONObj obj =
        BSON(MongosType::name("localhost:27017")
             << MongosType::created(Date_t::fromMillisSinceEpoch(1))
             << MongosType::ping(Date_t::fromMillisSinceEpoch(1)) << MongosType::uptime(100)
             << MongosType::mongoVersion("x.x.x") << MongosType::configVersion(0)
             << MongosType::advisoryHostFQDNs(BSONArrayBuilder().arr()));

    auto mongosTypeResult = MongosType::fromBSON(obj);
    ASSERT_EQ(ErrorCodes::NoSuchKey, mongosTypeResult.getStatus());
}

TEST(Validity, MissingMongoVersion) {
    BSONObj obj =
        BSON(MongosType::name("localhost:27017")
             << MongosType::created(Date_t::fromMillisSinceEpoch(1))
             << MongosType::ping(Date_t::fromMillisSinceEpoch(1)) << MongosType::uptime(100)
             << MongosType::waiting(false) << MongosType::configVersion(0)
             << MongosType::advisoryHostFQDNs(BSONArrayBuilder().arr()));

    auto mongosTypeResult = MongosType::fromBSON(obj);
    ASSERT_OK(mongosTypeResult.getStatus());

    MongosType& mtype = mongosTypeResult.getValue();
    /**
     * Note: mongoVersion should eventually become mandatory, but is optional now
     *       for backward compatibility reasons.
     */
    ASSERT_OK(mtype.validate());
}

TEST(Validity, MissingConfigVersion) {
    BSONObj obj =
        BSON(MongosType::name("localhost:27017")
             << MongosType::created(Date_t::fromMillisSinceEpoch(1))
             << MongosType::ping(Date_t::fromMillisSinceEpoch(1)) << MongosType::uptime(100)
             << MongosType::waiting(false) << MongosType::mongoVersion("x.x.x")
             << MongosType::advisoryHostFQDNs(BSONArrayBuilder().arr()));

    auto mongosTypeResult = MongosType::fromBSON(obj);
    ASSERT_OK(mongosTypeResult.getStatus());

    MongosType& mtype = mongosTypeResult.getValue();
    /**
     * Note: configVersion should eventually become mandatory, but is optional now
     *       for backward compatibility reasons.
     */
    ASSERT_OK(mtype.validate());
}

TEST(Validity, MissingAdvisoryHostFQDNs) {
    BSONObj obj = BSON(MongosType::name("localhost:27017")
                       << MongosType::created(Date_t::fromMillisSinceEpoch(1))
                       << MongosType::ping(Date_t::fromMillisSinceEpoch(1))
                       << MongosType::uptime(100) << MongosType::waiting(false)
                       << MongosType::mongoVersion("x.x.x") << MongosType::configVersion(0));

    auto mongosTypeResult = MongosType::fromBSON(obj);
    ASSERT_OK(mongosTypeResult.getStatus());

    MongosType& mType = mongosTypeResult.getValue();
    // advisoryHostFQDNs is optional
    ASSERT_OK(mType.validate());
}

TEST(Validity, EmptyAdvisoryHostFQDNs) {
    BSONObj obj = BSON(MongosType::name("localhost:27017")
                       << MongosType::created(Date_t::fromMillisSinceEpoch(1))
                       << MongosType::ping(Date_t::fromMillisSinceEpoch(1))
                       << MongosType::uptime(100) << MongosType::waiting(false)
                       << MongosType::mongoVersion("x.x.x") << MongosType::configVersion(0)
                       << MongosType::advisoryHostFQDNs(BSONArrayBuilder().arr()));

    auto mongosTypeResult = MongosType::fromBSON(obj);
    ASSERT_OK(mongosTypeResult.getStatus());

    MongosType& mType = mongosTypeResult.getValue();
    ASSERT_OK(mType.validate());

    ASSERT_EQUALS(mType.getAdvisoryHostFQDNs().size(), 0UL);
}

TEST(Validity, BadTypeAdvisoryHostFQDNs) {
    BSONObj obj = BSON(MongosType::name("localhost:27017")
                       << MongosType::created(Date_t::fromMillisSinceEpoch(1))
                       << MongosType::ping(Date_t::fromMillisSinceEpoch(1))
                       << MongosType::uptime(100) << MongosType::waiting(false)
                       << MongosType::mongoVersion("x.x.x") << MongosType::configVersion(0)
                       << MongosType::advisoryHostFQDNs(BSON_ARRAY("foo" << 0 << "baz")));

    auto mongosTypeResult = MongosType::fromBSON(obj);
    ASSERT_EQ(ErrorCodes::TypeMismatch, mongosTypeResult.getStatus());
}

TEST(Validity, Valid) {
    BSONObj obj = BSON(MongosType::name("localhost:27017")
                       << MongosType::created(Date_t::fromMillisSinceEpoch(1))
                       << MongosType::ping(Date_t::fromMillisSinceEpoch(1))
                       << MongosType::uptime(100) << MongosType::waiting(false)
                       << MongosType::mongoVersion("x.x.x") << MongosType::configVersion(0)
                       << MongosType::advisoryHostFQDNs(BSON_ARRAY("foo" << "bar"
                                                                         << "baz")));

    auto mongosTypeResult = MongosType::fromBSON(obj);
    ASSERT_OK(mongosTypeResult.getStatus());

    MongosType& mType = mongosTypeResult.getValue();
    ASSERT_OK(mType.validate());

    ASSERT_EQUALS(mType.getName(), "localhost:27017");
    ASSERT_EQUALS(mType.getPing(), Date_t::fromMillisSinceEpoch(1));
    ASSERT_EQUALS(mType.getUptime(), 100);
    ASSERT_EQUALS(mType.getWaiting(), false);
    ASSERT_EQUALS(mType.getMongoVersion(), "x.x.x");
    ASSERT_EQUALS(mType.getConfigVersion(), 0);
    ASSERT_EQUALS(mType.getAdvisoryHostFQDNs().size(), 3UL);
    ASSERT_EQUALS(mType.getAdvisoryHostFQDNs()[0], "foo");
    ASSERT_EQUALS(mType.getAdvisoryHostFQDNs()[1], "bar");
    ASSERT_EQUALS(mType.getAdvisoryHostFQDNs()[2], "baz");
}

TEST(Validity, BadType) {
    BSONObj obj = BSON(MongosType::name() << 0);

    auto mongosTypeResult = MongosType::fromBSON(obj);
    ASSERT_EQ(ErrorCodes::TypeMismatch, mongosTypeResult.getStatus());
}

}  // unnamed namespace
