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

#include "mongo/db/global_catalog/type_changelog.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
// (Generic FCV reference): used for testing, should exist across LTS binary versions
using GenericFCV = multiversion::GenericFCV;

class ChangeLogTypeTest : public unittest::Test {
public:
    BSONObj createChangeLogBSON(std::string missingField = "") {
        BSONObjBuilder builder;
        for (const auto& [fieldName, dv] : _dataAndValidatorMap) {
            if (fieldName != missingField) {
                builder.appendElements(dv.testValue);
            }
        }
        return builder.obj();
    }
    void assertExpectedChangeLogData(const ChangeLogType& changelog,
                                     std::string missingField = "") {
        for (const auto& [fieldName, dv] : _dataAndValidatorMap) {
            if (fieldName != missingField) {
                dv.assertTestValue(changelog);
            }
        }
    }

private:
    const std::string _kChangeId = "host.local-2012-11-21T19:14:10-8";
    const std::string _kServer = "host.local";
    const std::string _kShard = "shardname";
    const std::string _kClientAddr = "192.168.0.189:51128";
    const Date_t _kTime = Date_t::fromMillisSinceEpoch(1);
    const std::string _kWhat = "split";
    // (Generic FCV reference): used for testing, should exist across LTS binary versions
    const VersionContext _kVersionContext = VersionContext{GenericFCV::kLastLTS};
    const std::string _kNs = "test.test";
    const BSONObj _kDetails = BSON("dummy" << "info");

    struct ChangeLogFieldValidator {
        BSONObj testValue;
        std::function<void(const ChangeLogType&)> assertTestValue;
    };

    std::map<std::string, ChangeLogFieldValidator> _dataAndValidatorMap = {
        {"changeId",
         {BSON(ChangeLogType::changeId() << _kChangeId),
          [&](const ChangeLogType& changelog) {
              ASSERT_EQUALS(changelog.getChangeId(), _kChangeId);
          }}},
        {"server",
         {BSON(ChangeLogType::server() << _kServer),
          [&](const ChangeLogType& changelog) {
              ASSERT_EQUALS(changelog.getServer(), _kServer);
          }}},
        {"shard",
         {BSON(ChangeLogType::shard() << _kShard),
          [&](const ChangeLogType& changelog) {
              ASSERT_EQUALS(changelog.getShard(), _kShard);
          }}},
        {"clientAddr",
         {BSON(ChangeLogType::clientAddr() << _kClientAddr),
          [&](const ChangeLogType& changelog) {
              ASSERT_EQUALS(changelog.getClientAddr(), _kClientAddr);
          }}},
        {"time",
         {BSON(ChangeLogType::time() << _kTime),
          [&](const ChangeLogType& changelog) {
              ASSERT_EQUALS(changelog.getTime(), _kTime);
          }}},
        {"what",
         {BSON(ChangeLogType::what() << _kWhat),
          [&](const ChangeLogType& changelog) {
              ASSERT_EQUALS(changelog.getWhat(), _kWhat);
          }}},
        {"versionContext",
         {BSON(ChangeLogType::versionContext() << _kVersionContext.toBSON()),
          [&](const ChangeLogType& changelog) {
              ASSERT_EQUALS(changelog.getVersionContext().value(), _kVersionContext);
          }}},
        {"ns",
         {BSON(ChangeLogType::ns() << _kNs),
          [&](const ChangeLogType& changelog) {
              ASSERT_EQUALS(changelog.getNS(),
                            NamespaceString::createNamespaceString_forTest(boost::none, _kNs));
          }}},
        {"details",
         {BSON(ChangeLogType::details() << _kDetails), [&](const ChangeLogType& changelog) {
              ASSERT_BSONOBJ_EQ(changelog.getDetails(), _kDetails);
          }}}};
};

TEST_F(ChangeLogTypeTest, DeserializeEmpty) {
    auto changeLogResult = ChangeLogType::fromBSON(BSONObj());
    ASSERT_NOT_OK(changeLogResult.getStatus());
}

TEST_F(ChangeLogTypeTest, DeserializeValid) {
    BSONObj obj = createChangeLogBSON();
    auto changeLogResult = ChangeLogType::fromBSON(obj);
    ASSERT_OK(changeLogResult.getStatus());
    ChangeLogType& logEntry = changeLogResult.getValue();
    assertExpectedChangeLogData(logEntry);
}

TEST_F(ChangeLogTypeTest, DeserializeMissingRequiredFields) {
    std::vector<std::string> requiredFields{
        "changeId", "server", "clientAddr", "time", "what", "details"};
    for (const auto& fieldName : requiredFields) {
        BSONObj obj = createChangeLogBSON(fieldName);
        auto changeLogResult = ChangeLogType::fromBSON(obj);
        ASSERT_EQ(ErrorCodes::NoSuchKey, changeLogResult.getStatus());
    }
}

TEST_F(ChangeLogTypeTest, DeserializeMissingShard) {
    std::string missingField = "shard";
    BSONObj obj = createChangeLogBSON(missingField);
    auto changeLogResult = ChangeLogType::fromBSON(obj);
    ASSERT_OK(changeLogResult.getStatus());
    const auto& logEntry = changeLogResult.getValue();
    assertExpectedChangeLogData(logEntry, missingField);
    ASSERT_TRUE(logEntry.getShard().empty());
}

TEST_F(ChangeLogTypeTest, DeserializeMissingVCtx) {
    std::string missingField = "versionContext";
    BSONObj obj = createChangeLogBSON(missingField);
    auto changeLogResult = ChangeLogType::fromBSON(obj);
    ASSERT_OK(changeLogResult.getStatus());
    const auto& logEntry = changeLogResult.getValue();
    assertExpectedChangeLogData(logEntry, missingField);
    ASSERT_FALSE(logEntry.getVersionContext().has_value());
}

TEST_F(ChangeLogTypeTest, DeserializeMissingNS) {
    std::string missingField = "ns";
    BSONObj obj = createChangeLogBSON(missingField);
    auto changeLogResult = ChangeLogType::fromBSON(obj);
    ASSERT_OK(changeLogResult.getStatus());
    const auto& logEntry = changeLogResult.getValue();
    assertExpectedChangeLogData(logEntry, missingField);
    ASSERT_TRUE(logEntry.getNS().isEmpty());
}

TEST_F(ChangeLogTypeTest, DeserializeBadType) {
    ChangeLogType logEntry;
    BSONObj obj = BSON(ChangeLogType::changeId() << 0);
    auto changeLogResult = ChangeLogType::fromBSON(obj);
    ASSERT_EQ(ErrorCodes::TypeMismatch, changeLogResult.getStatus());
}

}  // namespace
}  // namespace mongo
