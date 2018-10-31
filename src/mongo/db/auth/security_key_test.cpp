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

#include "mongo/platform/basic.h"

#include "boost/filesystem.hpp"

#include "mongo/base/string_data.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/security_file.h"
#include "mongo/db/auth/security_key.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

class TestFile {
    TestFile(TestFile&) = delete;
    TestFile& operator=(TestFile&) = delete;

public:
    TestFile(StringData contents, bool fixPerms = true) : _path(boost::filesystem::unique_path()) {
        boost::filesystem::ofstream stream(_path, std::ios_base::out | std::ios_base::trunc);
        ASSERT_TRUE(stream.good());

        stream.write(contents.rawData(), contents.size());
        stream.close();
        if (fixPerms) {
            const auto perms = boost::filesystem::owner_read | boost::filesystem::owner_write;
            boost::filesystem::permissions(_path, perms);
        }
    }

    ~TestFile() {
        ASSERT_TRUE(boost::filesystem::remove(_path));
    }

    const boost::filesystem::path& path() const {
        return _path;
    }

private:
    boost::filesystem::path _path;
};

struct TestCase {
    enum class FailureMode { Success, Permissions, Parsing, SecurityKeyConstraint };

    TestCase(StringData contents_,
             std::initializer_list<std::string> expected_,
             FailureMode mode_ = FailureMode::Success)
        : fileContents(contents_.toString()), expected(expected_), mode(mode_) {}

    std::string fileContents;
    std::vector<std::string> expected;
    FailureMode mode = FailureMode::Success;
};

StringData longKeyMaker() {
    static const auto longKey = [] {
        std::array<char, 1026> ret;
        ret.fill('a');
        return ret;
    }();
    return StringData(longKey.data(), longKey.size());
}

std::initializer_list<TestCase> testCases = {
    // Our good ole insecure key
    {"foop de doop", {"foopdedoop"}},

    // Basic whitespace stripping gets done correctly
    {"foop\nde\ndoop", {"foopdedoop"}},

    // A more complex base64 character set key
    {"G92sqe/Y9Nn92fU1M8Q=cIKI", {"G92sqe/Y9Nn92fU1M8Q=cIKI"}},

    // A more complex base64 character set key with a ludicrous amount of whitespace
    {"G 9\n2\ts\rq\ne\n/\tY   9\rN\nn\r\n9\n2fU1M8Q=cIKI", {"G92sqe/Y9Nn92fU1M8Q=cIKI"}},

    // A more complex base64 character set key with YAML escaped whitespace in it
    {"\"G 9\\n2\\ts\\rq\\ne\\n/\\tY   9\\rN\\nn\\r\\n9\\n2fU1M8Q=cIKI\"",
     {"G92sqe/Y9Nn92fU1M8Q=cIKI"}},

    // An array of keys with ludicrous embedded whitespace in one (use the leading '-' array
    // YAML format)
    {"- \"foop de doop\"\n- \"G 9\\n2\\ts\\rq\\ne\\n/\\tY   9\\rN\\nn\\r\\n9\\n2fU1M8Q=cIKI\"",
     {"foopdedoop", "G92sqe/Y9Nn92fU1M8Q=cIKI"}},

    // An array of keys with the JSON-like YAML array format
    {"[ \"foop de doop\", \"other key\" ]", {"foopdedoop", "otherkey"}},

    // An empty file doesn't parse correctly
    {"", {}, TestCase::FailureMode::Parsing},

    // An empty array doesn't parse correctly
    {"[]", {}, TestCase::FailureMode::Parsing},

    // Invalid base64 characters don't parse correctly
    {"*xjy23`~/?", {}, TestCase::FailureMode::Parsing},

    // Raw binary data shouldn't parse correctly
    {"\x20\xfe\x04\x56\0\x34 foop de doop", {}, TestCase::FailureMode::Parsing},

    // A file with bad permissions doesn't parse correctly
    {"", {}, TestCase::FailureMode::Permissions},

    // These two keys should pass the security file parsing, but fail loading them as
    // security keys because they are too short or two long
    {"abc", {"abc"}, TestCase::FailureMode::SecurityKeyConstraint},
    {longKeyMaker(), {longKeyMaker().toString()}, TestCase::FailureMode::SecurityKeyConstraint}};

TEST(SecurityFile, Test) {
    for (const auto& testCase : testCases) {
        TestFile file(testCase.fileContents, testCase.mode != TestCase::FailureMode::Permissions);

        auto swKeys = readSecurityFile(file.path().string());
        if (testCase.mode == TestCase::FailureMode::Success ||
            testCase.mode == TestCase::FailureMode::SecurityKeyConstraint) {
            ASSERT_OK(swKeys.getStatus());
        } else {
            ASSERT_NOT_OK(swKeys.getStatus());
            continue;
        }

        auto keys = std::move(swKeys.getValue());
        ASSERT_EQ(keys.size(), testCase.expected.size());
        for (size_t i = 0; i < keys.size(); i++) {
            ASSERT_EQ(keys.at(i), testCase.expected.at(i));
        }
    }
}

TEST(SecurityKey, Test) {
    internalSecurity.user = std::make_shared<User>(UserName("__system", "local"));

    for (const auto& testCase : testCases) {
        TestFile file(testCase.fileContents, testCase.mode != TestCase::FailureMode::Permissions);

        if (testCase.mode == TestCase::FailureMode::Success) {
            ASSERT_TRUE(setUpSecurityKey(file.path().string()));
        } else {
            ASSERT_FALSE(setUpSecurityKey(file.path().string()));
        }
    }
}

}  // namespace mongo
