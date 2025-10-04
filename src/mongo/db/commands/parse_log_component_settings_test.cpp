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

#include "mongo/db/commands/parse_log_component_settings.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/logv2/log_component.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/unittest.h"

#include <climits>
#include <limits>
#include <memory>
#include <ostream>

namespace {

using namespace mongo;
using logv2::LogComponent;

typedef std::vector<LogComponentSetting> Settings;

TEST(Empty, Empty) {
    BSONObj input;
    StatusWith<Settings> result = parseLogComponentSettings(input);

    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(result.getValue().size(), 0u);
}

TEST(Flat, Numeric) {
    BSONObj input = BSON("verbosity" << 1);

    StatusWith<Settings> result = parseLogComponentSettings(input);

    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(result.getValue().size(), 1u);
    ASSERT_EQUALS(result.getValue()[0].level, 1);
    ASSERT_EQUALS(result.getValue()[0].component, LogComponent::kDefault);
}

TEST(Flat, FailNonNumeric) {
    BSONObj input = BSON("verbosity" << "not a number");

    StatusWith<Settings> result = parseLogComponentSettings(input);

    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::BadValue, result.getStatus().code());
    ASSERT_EQUALS(result.getStatus().reason(),
                  "Expected default.verbosity to be safely cast to integer, but could not: "
                  "Unable to coerce value to integral type");
}

TEST(Flat, FailNegative) {
    BSONObj input = BSON("verbosity" << -2);

    StatusWith<Settings> result = parseLogComponentSettings(input);

    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::BadValue, result.getStatus().code());
    ASSERT_EQUALS(result.getStatus().reason(),
                  "Expected default.verbosity to be greater than or equal to -1, but found -2");
}

TEST(Flat, FailMaxOutOfBoundsDouble) {
    BSONObj input = BSON("verbosity" << static_cast<double>(INT_MAX) + 1);

    StatusWith<Settings> result = parseLogComponentSettings(input);

    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::BadValue, result.getStatus().code());
    ASSERT_EQUALS(result.getStatus().reason(),
                  "Expected default.verbosity to be safely cast to integer, but could not: "
                  "Out of bounds coercing to integral value");
}

TEST(Flat, FailMaxOutOfBoundsLong) {
    BSONObj input = BSON("verbosity" << LLONG_MAX);

    StatusWith<Settings> result = parseLogComponentSettings(input);

    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::BadValue, result.getStatus().code());
    ASSERT_EQUALS(result.getStatus().reason(),
                  "Expected default.verbosity to be safely cast to integer, but could not: "
                  "Out of bounds coercing to integral value");
}

TEST(Flat, FailMinOutOfBoundsDouble) {
    BSONObj input = BSON("verbosity" << static_cast<double>(INT_MIN) - 1);

    StatusWith<Settings> result = parseLogComponentSettings(input);

    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::BadValue, result.getStatus().code());
    ASSERT_EQUALS(result.getStatus().reason(),
                  "Expected default.verbosity to be safely cast to integer, but could not: "
                  "Out of bounds coercing to integral value");
}

TEST(Flat, FailMinOutOfBoundsLong) {
    BSONObj input = BSON("verbosity" << LLONG_MIN);

    StatusWith<Settings> result = parseLogComponentSettings(input);

    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::BadValue, result.getStatus().code());
    ASSERT_EQUALS(result.getStatus().reason(),
                  "Expected default.verbosity to be safely cast to integer, but could not: "
                  "Out of bounds coercing to integral value");
}

TEST(Flat, FailNaN) {
    BSONObj input = BSON("verbosity" << std::numeric_limits<double>::quiet_NaN());

    StatusWith<Settings> result = parseLogComponentSettings(input);

    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::BadValue, result.getStatus().code());
    ASSERT_EQUALS(result.getStatus().reason(),
                  "Expected default.verbosity to be safely cast to integer, but could not: "
                  "Unable to coerce NaN/Inf to integral type");
}

TEST(Flat, FailBadComponent) {
    BSONObj input = BSON("NoSuchComponent" << 2);

    StatusWith<Settings> result = parseLogComponentSettings(input);

    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(result.getStatus().code(), ErrorCodes::BadValue);
    ASSERT_EQUALS(result.getStatus().reason(), "Invalid component name default.NoSuchComponent");
}

TEST(Nested, Numeric) {
    BSONObj input = BSON("accessControl" << BSON("verbosity" << 1));

    StatusWith<Settings> result = parseLogComponentSettings(input);

    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(result.getValue().size(), 1u);
    ASSERT_EQUALS(result.getValue()[0].level, 1);
    ASSERT_EQUALS(result.getValue()[0].component, LogComponent::kAccessControl);
}

TEST(Nested, FailNonNumeric) {
    BSONObj input = BSON("accessControl" << BSON("verbosity" << "Not a number"));

    StatusWith<Settings> result = parseLogComponentSettings(input);

    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(result.getStatus().code(), ErrorCodes::BadValue);
    std::stringstream ss;
    ss << "Expected accessControl.verbosity to be safely cast to integer, but could not: "
       << "Unable to coerce value to integral type";
    ASSERT_EQUALS(result.getStatus().reason(), ss.str());
}

TEST(Nested, FailNegative) {
    BSONObj input = BSON("accessControl" << BSON("verbosity" << -2));

    StatusWith<Settings> result = parseLogComponentSettings(input);

    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(result.getStatus().code(), ErrorCodes::BadValue);
    std::stringstream ss;
    ss << "Expected accessControl.verbosity to be greater than or equal to -1, but found -2";
    ASSERT_EQUALS(result.getStatus().reason(), ss.str());
}

TEST(Nested, FailMaxOutOfBoundsDouble) {
    BSONObj input = BSON("accessControl" << BSON("verbosity" << static_cast<double>(INT_MAX) + 1));

    StatusWith<Settings> result = parseLogComponentSettings(input);

    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(result.getStatus().code(), ErrorCodes::BadValue);
    std::stringstream ss;
    ss << "Expected accessControl.verbosity to be safely cast to integer, but could not: "
       << "Out of bounds coercing to integral value";
    ASSERT_EQUALS(result.getStatus().reason(), ss.str());
}

TEST(Nested, FailMaxOutOfBoundsLong) {
    BSONObj input = BSON("accessControl" << BSON("verbosity" << LLONG_MAX));

    StatusWith<Settings> result = parseLogComponentSettings(input);

    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(result.getStatus().code(), ErrorCodes::BadValue);
    std::stringstream ss;
    ss << "Expected accessControl.verbosity to be safely cast to integer, but could not: "
       << "Out of bounds coercing to integral value";
    ASSERT_EQUALS(result.getStatus().reason(), ss.str());
}

TEST(Nested, FailMinOutOfBoundsDouble) {
    BSONObj input = BSON("accessControl" << BSON("verbosity" << static_cast<double>(INT_MIN) - 1));

    StatusWith<Settings> result = parseLogComponentSettings(input);

    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(result.getStatus().code(), ErrorCodes::BadValue);
    std::stringstream ss;
    ss << "Expected accessControl.verbosity to be safely cast to integer, but could not: "
       << "Out of bounds coercing to integral value";
    ASSERT_EQUALS(result.getStatus().reason(), ss.str());
}

TEST(Nested, FailMinOutOfBoundsLong) {
    BSONObj input = BSON("accessControl" << BSON("verbosity" << LLONG_MIN));

    StatusWith<Settings> result = parseLogComponentSettings(input);

    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(result.getStatus().code(), ErrorCodes::BadValue);
    std::stringstream ss;
    ss << "Expected accessControl.verbosity to be safely cast to integer, but could not: "
       << "Out of bounds coercing to integral value";
    ASSERT_EQUALS(result.getStatus().reason(), ss.str());
}

TEST(Nested, FailNaN) {
    BSONObj input =
        BSON("accessControl" << BSON("verbosity" << std::numeric_limits<double>::quiet_NaN()));

    StatusWith<Settings> result = parseLogComponentSettings(input);

    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(result.getStatus().code(), ErrorCodes::BadValue);
    std::stringstream ss;
    ss << "Expected accessControl.verbosity to be safely cast to integer, but could not: "
       << "Unable to coerce NaN/Inf to integral type";
    ASSERT_EQUALS(result.getStatus().reason(), ss.str());
}

TEST(Nested, FailBadComponent) {
    BSONObj input = BSON("NoSuchComponent" << BSON("verbosity" << 2));

    StatusWith<Settings> result = parseLogComponentSettings(input);

    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(result.getStatus().code(), ErrorCodes::BadValue);
    ASSERT_EQUALS(result.getStatus().reason(), "Invalid component name default.NoSuchComponent");
}

TEST(Multi, Numeric) {
    BSONObj input =
        BSON("verbosity" << 2 << "accessControl" << BSON("verbosity" << 0) << "storage"
                         << BSON("verbosity" << 3 << "journal" << BSON("verbosity" << 5)));

    StatusWith<Settings> result = parseLogComponentSettings(input);

    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(result.getValue().size(), 4u);

    ASSERT_EQUALS(result.getValue()[0].level, 2);
    ASSERT_EQUALS(result.getValue()[0].component, LogComponent::kDefault);
    ASSERT_EQUALS(result.getValue()[1].level, 0);
    ASSERT_EQUALS(result.getValue()[1].component, LogComponent::kAccessControl);
    ASSERT_EQUALS(result.getValue()[2].level, 3);
    ASSERT_EQUALS(result.getValue()[2].component, LogComponent::kStorage);
    ASSERT_EQUALS(result.getValue()[3].level, 5);
    ASSERT_EQUALS(result.getValue()[3].component, LogComponent::kJournal);
}

TEST(Multi, FailBadComponent) {
    BSONObj input =
        BSON("verbosity" << 6 << "accessControl" << BSON("verbosity" << 5) << "storage"
                         << BSON("verbosity" << 4 << "journal" << BSON("verbosity" << 6))
                         << "No Such Component" << BSON("verbosity" << 2) << "extrafield" << 123);

    StatusWith<Settings> result = parseLogComponentSettings(input);

    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(result.getStatus().code(), ErrorCodes::BadValue);
    ASSERT_EQUALS(result.getStatus().reason(), "Invalid component name default.No Such Component");
}

TEST(DeeplyNested, FailFast) {
    BSONObj input =
        BSON("storage" << BSON("this" << BSON("is" << BSON("nested" << BSON("too" << "deeply")))));

    StatusWith<Settings> result = parseLogComponentSettings(input);

    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(result.getStatus().code(), ErrorCodes::BadValue);
    ASSERT_EQUALS(result.getStatus().reason(), "Invalid component name storage.this");
}

TEST(DeeplyNested, FailLast) {
    BSONObj input = BSON("storage" << BSON("journal" << BSON("No Such Component" << "bad")));

    StatusWith<Settings> result = parseLogComponentSettings(input);

    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(result.getStatus().code(), ErrorCodes::BadValue);
    ASSERT_EQUALS(result.getStatus().reason(),
                  "Invalid component name storage.journal.No Such Component");
}
}  // namespace
