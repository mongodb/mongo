// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/rpc/object_check.h"

#include "mongo/base/data_range_cursor.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/server_options.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/scopeguard.h"

#include <iterator>
#include <variant>


namespace mongo::rpc {
namespace {

using std::begin;
using std::end;

TEST(DataTypeValidated, BSONValidationEnabled) {
    bool wasEnabled = serverGlobalParams.objcheck;
    const auto setValidation = [&](bool enabled) {
        serverGlobalParams.objcheck = enabled;
    };
    ON_BLOCK_EXIT([=] { setValidation(wasEnabled); });

    BSONObj valid = BSON("baz" << "bar"
                               << "garply" << BSON("foo" << "bar"));
    char buf[1024] = {0};
    std::copy(valid.objdata(), valid.objdata() + valid.objsize(), begin(buf));
    {
        ValidatedBSONObj v;
        ConstDataRangeCursor cdrc(begin(buf), end(buf));
        ASSERT_OK(cdrc.readAndAdvanceNoThrow(&v));
    }

    {
        // mess up the data
        DataRangeCursor drc(begin(buf), end(buf));
        // skip past size so we don't trip any sanity checks.
        drc.advanceNoThrow(4).transitional_ignore();  // skip size
        while (drc.writeAndAdvanceNoThrow(0xFF).isOK())
            ;
    }

    {
        ValidatedBSONObj v;
        ConstDataRangeCursor cdrc(begin(buf), end(buf));
        ASSERT_NOT_OK(cdrc.readAndAdvanceNoThrow(&v));
    }

    {
        // disable validation
        setValidation(false);
        ValidatedBSONObj v;
        ConstDataRangeCursor cdrc(begin(buf), end(buf));
        ASSERT_OK(cdrc.readAndAdvanceNoThrow(&v));
    }
}

DEATH_TEST(ObjectCheckDeathTest, BSONValidationEnabledWithCrashOnError, "50761") {
    bool objcheckValue = serverGlobalParams.objcheck;
    serverGlobalParams.objcheck = true;
    ON_BLOCK_EXIT([=] { serverGlobalParams.objcheck = objcheckValue; });

    bool crashOnErrorValue = serverGlobalParams.crashOnInvalidBSONError;
    serverGlobalParams.crashOnInvalidBSONError = true;
    ON_BLOCK_EXIT([&] { serverGlobalParams.crashOnInvalidBSONError = crashOnErrorValue; });

    BSONObj valid = BSON("baz" << "bar"
                               << "garply" << BSON("foo" << "bar"));
    char buf[1024] = {0};
    std::copy(valid.objdata(), valid.objdata() + valid.objsize(), begin(buf));

    {
        // mess up the data
        DataRangeCursor drc(begin(buf), end(buf));
        // skip past size so we don't trip any sanity checks.
        drc.advanceNoThrow(4).transitional_ignore();  // skip size
        while (drc.writeAndAdvanceNoThrow(0xFF).isOK())
            ;
    }

    {
        ValidatedBSONObj v;
        ConstDataRangeCursor cdrc(begin(buf), end(buf));
        // Crashes because of invalid BSON
        cdrc.readAndAdvanceNoThrow(&v).ignore();
    }
}

}  // namespace
}  // namespace mongo::rpc
