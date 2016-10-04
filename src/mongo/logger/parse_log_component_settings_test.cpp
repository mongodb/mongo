/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/logger/log_component.h"
#include "mongo/logger/parse_log_component_settings.h"

#include "mongo/db/jsobj.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;
using namespace mongo::logger;

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
    BSONObj input = BSON("verbosity"
                         << "not a number");

    StatusWith<Settings> result = parseLogComponentSettings(input);

    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(ErrorCodes::BadValue, result.getStatus().code());
    ASSERT_EQUALS(result.getStatus().reason(),
                  "Expected default.verbosity to be a number, but found string");
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
    BSONObj input = BSON("accessControl" << BSON("verbosity"
                                                 << "Not a number"));

    StatusWith<Settings> result = parseLogComponentSettings(input);

    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(result.getStatus().code(), ErrorCodes::BadValue);
    ASSERT_EQUALS(result.getStatus().reason(),
                  "Expected accessControl.verbosity to be a number, but found string");
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
                         << "No Such Component"
                         << BSON("verbosity" << 2)
                         << "extrafield"
                         << 123);

    StatusWith<Settings> result = parseLogComponentSettings(input);

    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(result.getStatus().code(), ErrorCodes::BadValue);
    ASSERT_EQUALS(result.getStatus().reason(), "Invalid component name default.No Such Component");
}

TEST(DeeplyNested, FailFast) {
    BSONObj input =
        BSON("storage" << BSON("this" << BSON("is" << BSON("nested" << BSON("too"
                                                                            << "deeply")))));

    StatusWith<Settings> result = parseLogComponentSettings(input);

    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(result.getStatus().code(), ErrorCodes::BadValue);
    ASSERT_EQUALS(result.getStatus().reason(), "Invalid component name storage.this");
}

TEST(DeeplyNested, FailLast) {
    BSONObj input = BSON("storage" << BSON("journal" << BSON("No Such Component"
                                                             << "bad")));

    StatusWith<Settings> result = parseLogComponentSettings(input);

    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQUALS(result.getStatus().code(), ErrorCodes::BadValue);
    ASSERT_EQUALS(result.getStatus().reason(),
                  "Invalid component name storage.journal.No Such Component");
}
}
