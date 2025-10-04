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

#include "mongo/s/request_types/migration_secondary_throttle_options.h"

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"

#include <cstdint>
#include <memory>
#include <variant>

#include <fmt/format.h>

namespace mongo {

using unittest::assertGet;

namespace {

TEST(MigrationSecondaryThrottleOptions, CreateDefault) {
    MigrationSecondaryThrottleOptions options =
        MigrationSecondaryThrottleOptions::create(MigrationSecondaryThrottleOptions::kDefault);
    ASSERT_EQ(MigrationSecondaryThrottleOptions::kDefault, options.getSecondaryThrottle());
    ASSERT(!options.isWriteConcernSpecified());
    ASSERT_BSONOBJ_EQ(BSONObj(), options.toBSON());
}

TEST(MigrationSecondaryThrottleOptions, CreateOn) {
    MigrationSecondaryThrottleOptions options =
        MigrationSecondaryThrottleOptions::create(MigrationSecondaryThrottleOptions::kOn);
    ASSERT_EQ(MigrationSecondaryThrottleOptions::kOn, options.getSecondaryThrottle());
    ASSERT(!options.isWriteConcernSpecified());
    ASSERT_BSONOBJ_EQ(BSON("secondaryThrottle" << true), options.toBSON());
}

TEST(MigrationSecondaryThrottleOptions, CreateOff) {
    MigrationSecondaryThrottleOptions options =
        MigrationSecondaryThrottleOptions::create(MigrationSecondaryThrottleOptions::kOff);
    ASSERT_EQ(MigrationSecondaryThrottleOptions::kOff, options.getSecondaryThrottle());
    ASSERT(!options.isWriteConcernSpecified());
    ASSERT_BSONOBJ_EQ(BSON("secondaryThrottle" << false), options.toBSON());
}

TEST(MigrationSecondaryThrottleOptions, NotSpecifiedInCommandBSON) {
    MigrationSecondaryThrottleOptions options = assertGet(
        MigrationSecondaryThrottleOptions::createFromCommand(BSON("someOtherField" << 1)));
    ASSERT_EQ(MigrationSecondaryThrottleOptions::kDefault, options.getSecondaryThrottle());
    ASSERT(!options.isWriteConcernSpecified());
    ASSERT_BSONOBJ_EQ(BSONObj(), options.toBSON());
}

TEST(MigrationSecondaryThrottleOptions, EnabledInCommandBSONWithoutWriteConcern) {
    MigrationSecondaryThrottleOptions options =
        assertGet(MigrationSecondaryThrottleOptions::createFromCommand(
            BSON("someOtherField" << 1 << "secondaryThrottle" << true)));
    ASSERT_EQ(MigrationSecondaryThrottleOptions::kOn, options.getSecondaryThrottle());
    ASSERT(!options.isWriteConcernSpecified());
}

TEST(MigrationSecondaryThrottleOptions, EnabledInCommandBSONWithSimpleWriteConcern) {
    MigrationSecondaryThrottleOptions options =
        assertGet(MigrationSecondaryThrottleOptions::createFromCommand(
            BSON("someOtherField" << 1 << "secondaryThrottle" << true << "writeConcern"
                                  << BSON("w" << 2))));
    ASSERT_EQ(MigrationSecondaryThrottleOptions::kOn, options.getSecondaryThrottle());
    ASSERT(options.isWriteConcernSpecified());

    WriteConcernOptions writeConcern = options.getWriteConcern();
    ASSERT(holds_alternative<int64_t>(writeConcern.w));
    ASSERT_EQ(2, get<int64_t>(writeConcern.w));
    ASSERT_EQ(static_cast<int>(WriteConcernOptions::SyncMode::UNSET),
              static_cast<int>(writeConcern.syncMode));
    ASSERT_EQ(WriteConcernOptions::kNoTimeout, writeConcern.wTimeout);
}

TEST(MigrationSecondaryThrottleOptions, EnabledInCommandBSONWithCompleteWriteConcern) {
    MigrationSecondaryThrottleOptions options =
        assertGet(MigrationSecondaryThrottleOptions::createFromCommand(
            BSON("someOtherField" << 1 << "secondaryThrottle" << true << "writeConcern"
                                  << BSON("w" << 3 << "j" << true))));
    ASSERT_EQ(MigrationSecondaryThrottleOptions::kOn, options.getSecondaryThrottle());
    ASSERT(options.isWriteConcernSpecified());

    WriteConcernOptions writeConcern = options.getWriteConcern();
    ASSERT(holds_alternative<int64_t>(writeConcern.w));
    ASSERT_EQ(3, get<int64_t>(writeConcern.w));
    ASSERT_EQ(static_cast<int>(WriteConcernOptions::SyncMode::JOURNAL),
              static_cast<int>(writeConcern.syncMode));
    ASSERT_EQ(WriteConcernOptions::kNoTimeout, writeConcern.wTimeout);
}

TEST(MigrationSecondaryThrottleOptions, DisabledInCommandBSON) {
    MigrationSecondaryThrottleOptions options =
        assertGet(MigrationSecondaryThrottleOptions::createFromCommand(
            BSON("someOtherField" << 1 << "secondaryThrottle" << false)));
    ASSERT_EQ(MigrationSecondaryThrottleOptions::kOff, options.getSecondaryThrottle());
}

TEST(MigrationSecondaryThrottleOptions, NotSpecifiedInBalancerConfig) {
    MigrationSecondaryThrottleOptions options = assertGet(
        MigrationSecondaryThrottleOptions::createFromBalancerConfig(BSON("someOtherField" << 1)));
    ASSERT_EQ(MigrationSecondaryThrottleOptions::kDefault, options.getSecondaryThrottle());
    ASSERT(!options.isWriteConcernSpecified());
    ASSERT_BSONOBJ_EQ(BSONObj(), options.toBSON());
}

TEST(MigrationSecondaryThrottleOptions, EnabledInBalancerConfigLegacyStyle) {
    MigrationSecondaryThrottleOptions options =
        assertGet(MigrationSecondaryThrottleOptions::createFromBalancerConfig(
            BSON("someOtherField" << 1 << "_secondaryThrottle" << true)));
    ASSERT_EQ(MigrationSecondaryThrottleOptions::kOn, options.getSecondaryThrottle());
    ASSERT(!options.isWriteConcernSpecified());
}

TEST(MigrationSecondaryThrottleOptions, EnabledInBalancerConfigWithSimpleWriteConcern) {
    MigrationSecondaryThrottleOptions options =
        assertGet(MigrationSecondaryThrottleOptions::createFromBalancerConfig(
            BSON("someOtherField" << 1 << "_secondaryThrottle" << BSON("w" << 2))));
    ASSERT_EQ(MigrationSecondaryThrottleOptions::kOn, options.getSecondaryThrottle());
    ASSERT(options.isWriteConcernSpecified());

    WriteConcernOptions writeConcern = options.getWriteConcern();
    ASSERT(holds_alternative<int64_t>(writeConcern.w));
    ASSERT_EQ(2, get<int64_t>(writeConcern.w));
    ASSERT_EQ(static_cast<int>(WriteConcernOptions::SyncMode::UNSET),
              static_cast<int>(writeConcern.syncMode));
    ASSERT_EQ(WriteConcernOptions::kNoTimeout, writeConcern.wTimeout);
}

TEST(MigrationSecondaryThrottleOptions, EnabledInBalancerConfigWithCompleteWriteConcern) {
    MigrationSecondaryThrottleOptions options =
        assertGet(MigrationSecondaryThrottleOptions::createFromBalancerConfig(
            BSON("someOtherField" << 1 << "_secondaryThrottle" << BSON("w" << 3 << "j" << true))));
    ASSERT_EQ(MigrationSecondaryThrottleOptions::kOn, options.getSecondaryThrottle());
    ASSERT(options.isWriteConcernSpecified());

    WriteConcernOptions writeConcern = options.getWriteConcern();
    ASSERT(holds_alternative<int64_t>(writeConcern.w));
    ASSERT_EQ(3, get<int64_t>(writeConcern.w));
    ASSERT_EQ(static_cast<int>(WriteConcernOptions::SyncMode::JOURNAL),
              static_cast<int>(writeConcern.syncMode));
    ASSERT_EQ(WriteConcernOptions::kNoTimeout, writeConcern.wTimeout);
}

TEST(MigrationSecondaryThrottleOptions, DisabledInBalancerConfig) {
    MigrationSecondaryThrottleOptions options =
        assertGet(MigrationSecondaryThrottleOptions::createFromCommand(
            BSON("someOtherField" << 1 << "_secondaryThrottle" << false)));
    ASSERT_EQ(MigrationSecondaryThrottleOptions::kOff, options.getSecondaryThrottle());
}

TEST(MigrationSecondaryThrottleOptions, IgnoreWriteConcernWhenSecondaryThrottleOff) {
    MigrationSecondaryThrottleOptions options =
        assertGet(MigrationSecondaryThrottleOptions::createFromCommand(
            BSON("someOtherField" << 1 << "_secondaryThrottle" << false << "writeConcern"
                                  << BSON("w" << "majority"))));
    ASSERT_EQ(MigrationSecondaryThrottleOptions::kOff, options.getSecondaryThrottle());
    ASSERT(!options.isWriteConcernSpecified());
}

TEST(MigrationSecondaryThrottleOptions, IgnoreWriteConcernWhenSecondaryThrottleAbsent) {
    MigrationSecondaryThrottleOptions options =
        assertGet(MigrationSecondaryThrottleOptions::createFromCommand(
            BSON("someOtherField" << 1 << "writeConcern" << BSON("w" << "majority"))));
    ASSERT_EQ(MigrationSecondaryThrottleOptions::kDefault, options.getSecondaryThrottle());
    ASSERT(!options.isWriteConcernSpecified());
}

TEST(MigrationSecondaryThrottleOptions, EqualityOperatorSameValue) {
    auto value1 = MigrationSecondaryThrottleOptions::createWithWriteConcern(WriteConcernOptions{
        "majority", WriteConcernOptions::SyncMode::JOURNAL, Milliseconds{30000}});
    auto value2 = MigrationSecondaryThrottleOptions::createWithWriteConcern(WriteConcernOptions{
        "majority", WriteConcernOptions::SyncMode::JOURNAL, Milliseconds{30000}});

    ASSERT(value1 == value2);
}

TEST(MigrationSecondaryThrottleOptions, EqualityOperatorDifferentValues) {
    auto value1 = MigrationSecondaryThrottleOptions::createWithWriteConcern(WriteConcernOptions{
        "majority", WriteConcernOptions::SyncMode::JOURNAL, Milliseconds{30000}});
    auto value2 = MigrationSecondaryThrottleOptions::createWithWriteConcern(WriteConcernOptions{
        "majority", WriteConcernOptions::SyncMode::JOURNAL, Milliseconds{60000}});

    ASSERT(!(value1 == value2));
}

}  // namespace
}  // namespace mongo
