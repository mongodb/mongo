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

#include <fstream>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/client/mongo_uri.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"

#include <boost/filesystem/operations.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional_io.hpp>

namespace mongo {
namespace {
using transport::ConnectSSLMode;
constexpr auto kEnableSSL = ConnectSSLMode::kEnableSSL;
constexpr auto kDisableSSL = ConnectSSLMode::kDisableSSL;
constexpr auto kGlobalSSLMode = ConnectSSLMode::kGlobalSSLMode;

struct URITestCase {
    std::string URI;
    std::string uname;
    std::string password;
    ConnectionString::ConnectionType type;
    std::string setname;
    size_t numservers;
    MongoURI::OptionsMap options;
    std::string database;
    ConnectSSLMode sslMode;
};

struct InvalidURITestCase {
    std::string URI;
    boost::optional<ErrorCodes::Error> code;
    InvalidURITestCase(std::string aURI, boost::optional<ErrorCodes::Error> aCode = boost::none) {
        URI = std::move(aURI);
        code = std::move(aCode);
    }
};

void compareOptions(size_t lineNumber,
                    StringData uri,
                    const MongoURI::OptionsMap& connection,
                    const MongoURI::OptionsMap& expected) {
    std::vector<std::pair<MongoURI::CaseInsensitiveString, std::string>> options(begin(connection),
                                                                                 end(connection));
    std::sort(begin(options), end(options));
    std::vector<std::pair<MongoURI::CaseInsensitiveString, std::string>> expectedOptions(
        begin(expected), end(expected));
    std::sort(begin(expectedOptions), end(expectedOptions));

    for (std::size_t i = 0; i < std::min(options.size(), expectedOptions.size()); ++i) {
        if (options[i] != expectedOptions[i]) {
            unittest::log() << "Option: \"tolower(" << options[i].first.original()
                            << ")=" << options[i].second << "\" doesn't equal: \"tolower("
                            << expectedOptions[i].first.original()
                            << ")=" << expectedOptions[i].second << "\""
                            << " data on line: " << lineNumber << std::endl;
            std::cerr << "Failing URI: \"" << uri << "\""
                      << " data on line: " << lineNumber << std::endl;
            ASSERT(false);
        }
    }
    ASSERT_EQ(options.size(), expectedOptions.size()) << "Failing URI: "
                                                      << " data on line: " << lineNumber << uri;
}

const ConnectionString::ConnectionType kMaster = ConnectionString::MASTER;
const ConnectionString::ConnectionType kSet = ConnectionString::SET;

const URITestCase validCases[] = {

    {"mongodb://user:pwd@127.0.0.1", "user", "pwd", kMaster, "", 1, {}, "", kGlobalSSLMode},

    {"mongodb://user@127.0.0.1", "user", "", kMaster, "", 1, {}, "", kGlobalSSLMode},

    {"mongodb://localhost/?foo=bar", "", "", kMaster, "", 1, {{"foo", "bar"}}, "", kGlobalSSLMode},

    {"mongodb://localhost,/?foo=bar", "", "", kMaster, "", 1, {{"foo", "bar"}}, "", kGlobalSSLMode},

    {"mongodb://user:pwd@127.0.0.1:1234", "user", "pwd", kMaster, "", 1, {}, "", kGlobalSSLMode},

    {"mongodb://user@127.0.0.1:1234", "user", "", kMaster, "", 1, {}, "", kGlobalSSLMode},

    {"mongodb://127.0.0.1:1234/dbName?foo=a&c=b",
     "",
     "",
     kMaster,
     "",
     1,
     {{"foo", "a"}, {"c", "b"}},
     "dbName",
     kGlobalSSLMode},

    {"mongodb://127.0.0.1/dbName?foo=a&c=b",
     "",
     "",
     kMaster,
     "",
     1,
     {{"foo", "a"}, {"c", "b"}},
     "dbName",
     kGlobalSSLMode},

    {"mongodb://user:pwd@127.0.0.1,/dbName?foo=a&c=b",
     "user",
     "pwd",
     kMaster,
     "",
     1,
     {{"foo", "a"}, {"c", "b"}},
     "dbName",
     kGlobalSSLMode},

    {"mongodb://user:pwd@127.0.0.1,127.0.0.2/dbname?a=b&replicaSet=replName",
     "user",
     "pwd",
     kSet,
     "replName",
     2,
     {{"a", "b"}, {"replicaSet", "replName"}},
     "dbname",
     kGlobalSSLMode},

    {"mongodb://needs%20encoding%25%23!%3C%3E:pwd@127.0.0.1,127.0.0.2/"
     "dbname?a=b&replicaSet=replName",
     "needs encoding%#!<>",
     "pwd",
     kSet,
     "replName",
     2,
     {{"a", "b"}, {"replicaSet", "replName"}},
     "dbname",
     kGlobalSSLMode},

    {"mongodb://needs%20encoding%25%23!%3C%3E:pwd@127.0.0.1,127.0.0.2/"
     "db@name?a=b&replicaSet=replName",
     "needs encoding%#!<>",
     "pwd",
     kSet,
     "replName",
     2,
     {{"a", "b"}, {"replicaSet", "replName"}},
     "db@name",
     kGlobalSSLMode},

    {"mongodb://user:needs%20encoding%25%23!%3C%3E@127.0.0.1,127.0.0.2/"
     "dbname?a=b&replicaSet=replName",
     "user",
     "needs encoding%#!<>",
     kSet,
     "replName",
     2,
     {{"a", "b"}, {"replicaSet", "replName"}},
     "dbname",
     kGlobalSSLMode},

    {"mongodb://user:pwd@127.0.0.1,127.0.0.2/dbname?a=b&replicaSet=needs%20encoding%25%23!%3C%3E",
     "user",
     "pwd",
     kSet,
     "needs encoding%#!<>",
     2,
     {{"a", "b"}, {"replicaSet", "needs encoding%#!<>"}},
     "dbname",
     kGlobalSSLMode},

    {"mongodb://user:pwd@127.0.0.1,127.0.0.2/needsencoding%40hello?a=b&replicaSet=replName",
     "user",
     "pwd",
     kSet,
     "replName",
     2,
     {{"a", "b"}, {"replicaSet", "replName"}},
     "needsencoding@hello",
     kGlobalSSLMode},

    {"mongodb://user:pwd@127.0.0.1,127.0.0.2/?replicaSet=replName",
     "user",
     "pwd",
     kSet,
     "replName",
     2,
     {{"replicaSet", "replName"}},
     "",
     kGlobalSSLMode},

    {"mongodb://user@127.0.0.1,127.0.0.2/?replicaSet=replName",
     "user",
     "",
     kSet,
     "replName",
     2,
     {{"replicaSet", "replName"}},
     "",
     kGlobalSSLMode},

    {"mongodb://user@127.0.0.1,127.0.0.2/?replicaset=replName",
     "user",
     "",
     kSet,
     "replName",
     2,
     {{"replicaSet", "replName"}},
     "",
     kGlobalSSLMode},

    {"mongodb://127.0.0.1,127.0.0.2/dbName?foo=a&c=b&replicaSet=replName",
     "",
     "",
     kSet,
     "replName",
     2,
     {{"foo", "a"}, {"c", "b"}, {"replicaSet", "replName"}},
     "dbName",
     kGlobalSSLMode},

    {"mongodb://user:pwd@127.0.0.1:1234,127.0.0.2:1234/?replicaSet=replName",
     "user",
     "pwd",
     kSet,
     "replName",
     2,
     {{"replicaSet", "replName"}},
     "",
     kGlobalSSLMode},

    {"mongodb://user@127.0.0.1:1234,127.0.0.2:1234/?replicaSet=replName",
     "user",
     "",
     kSet,
     "replName",
     2,
     {{"replicaSet", "replName"}},
     "",
     kGlobalSSLMode},

    {"mongodb://127.0.0.1:1234,127.0.0.1:1234/dbName?foo=a&c=b&replicaSet=replName",
     "",
     "",
     kSet,
     "replName",
     2,
     {{"foo", "a"}, {"c", "b"}, {"replicaSet", "replName"}},
     "dbName",
     kGlobalSSLMode},

    {"mongodb://user:pwd@[::1]", "user", "pwd", kMaster, "", 1, {}, "", kGlobalSSLMode},

    {"mongodb://user@[::1]", "user", "", kMaster, "", 1, {}, "", kGlobalSSLMode},

    {"mongodb://[::1]/dbName?foo=a&c=b",
     "",
     "",
     kMaster,
     "",
     1,
     {{"foo", "a"}, {"c", "b"}},
     "dbName",
     kGlobalSSLMode},

    {"mongodb://user:pwd@[::1]:1234", "user", "pwd", kMaster, "", 1, {}, "", kGlobalSSLMode},

    {"mongodb://user@[::1]:1234", "user", "", kMaster, "", 1, {}, "", kGlobalSSLMode},

    {"mongodb://[::1]:1234/dbName?foo=a&c=b",
     "",
     "",
     kMaster,
     "",
     1,
     {{"foo", "a"}, {"c", "b"}},
     "dbName",
     kGlobalSSLMode},

    {"mongodb://user:pwd@[::1],127.0.0.2/?replicaSet=replName",
     "user",
     "pwd",
     kSet,
     "replName",
     2,
     {{"replicaSet", "replName"}},
     "",
     kGlobalSSLMode},

    {"mongodb://user@[::1],127.0.0.2/?replicaSet=replName",
     "user",
     "",
     kSet,
     "replName",
     2,
     {{"replicaSet", "replName"}},
     "",
     kGlobalSSLMode},

    {"mongodb://[::1],127.0.0.2/dbName?foo=a&c=b&replicaSet=replName",
     "",
     "",
     kSet,
     "replName",
     2,
     {{"foo", "a"}, {"c", "b"}, {"replicaSet", "replName"}},
     "dbName",
     kGlobalSSLMode},

    {"mongodb://user:pwd@[::1]:1234,127.0.0.2:1234/?replicaSet=replName",
     "user",
     "pwd",
     kSet,
     "replName",
     2,
     {{"replicaSet", "replName"}},
     "",
     kGlobalSSLMode},

    {"mongodb://user@[::1]:1234,127.0.0.2:1234/?replicaSet=replName",
     "user",
     "",
     kSet,
     "replName",
     2,
     {{"replicaSet", "replName"}},
     "",
     kGlobalSSLMode},

    {"mongodb://[::1]:1234,[::1]:1234/dbName?foo=a&c=b&replicaSet=replName",
     "",
     "",
     kSet,
     "replName",
     2,
     {{"foo", "a"}, {"c", "b"}, {"replicaSet", "replName"}},
     "dbName",
     kGlobalSSLMode},

    {"mongodb://user:pwd@[::1]", "user", "pwd", kMaster, "", 1, {}, "", kGlobalSSLMode},

    {"mongodb://user@[::1]", "user", "", kMaster, "", 1, {}, "", kGlobalSSLMode},

    {"mongodb://[::1]/dbName?foo=a&c=b",
     "",
     "",
     kMaster,
     "",
     1,
     {{"foo", "a"}, {"c", "b"}},
     "dbName",
     kGlobalSSLMode},

    {"mongodb://user:pwd@[::1]:1234", "user", "pwd", kMaster, "", 1, {}, "", kGlobalSSLMode},

    {"mongodb://user@[::1]:1234", "user", "", kMaster, "", 1, {}, "", kGlobalSSLMode},

    {"mongodb://[::1]:1234/dbName?foo=a&c=b",
     "",
     "",
     kMaster,
     "",
     1,
     {{"foo", "a"}, {"c", "b"}},
     "dbName",
     kGlobalSSLMode},

    {"mongodb://user:pwd@[::1],127.0.0.2/?replicaSet=replName",
     "user",
     "pwd",
     kSet,
     "replName",
     2,
     {{"replicaSet", "replName"}},
     "",
     kGlobalSSLMode},

    {"mongodb://user@[::1],127.0.0.2/?replicaSet=replName",
     "user",
     "",
     kSet,
     "replName",
     2,
     {{"replicaSet", "replName"}},
     "",
     kGlobalSSLMode},

    {"mongodb://[::1],127.0.0.2/dbName?foo=a&c=b&replicaSet=replName",
     "",
     "",
     kSet,
     "replName",
     2,
     {{"foo", "a"}, {"c", "b"}, {"replicaSet", "replName"}},
     "dbName",
     kGlobalSSLMode},

    {"mongodb://user:pwd@[::1]:1234,127.0.0.2:1234/?replicaSet=replName",
     "user",
     "pwd",
     kSet,
     "replName",
     2,
     {{"replicaSet", "replName"}},
     "",
     kGlobalSSLMode},

    {"mongodb://user@[::1]:1234,127.0.0.2:1234/?replicaSet=replName",
     "user",
     "",
     kSet,
     "replName",
     2,
     {{"replicaSet", "replName"}},
     "",
     kGlobalSSLMode},

    {"mongodb://[::1]:1234,[::1]:1234/dbName?foo=a&c=b&replicaSet=replName",
     "",
     "",
     kSet,
     "replName",
     2,
     {{"foo", "a"}, {"c", "b"}, {"replicaSet", "replName"}},
     "dbName",
     kGlobalSSLMode},

    {"mongodb://user:pwd@[::1]/?authMechanism=GSSAPI&authMechanismProperties=SERVICE_NAME:foobar",
     "user",
     "pwd",
     kMaster,
     "",
     1,
     {{"authmechanism", "GSSAPI"}, {"authmechanismproperties", "SERVICE_NAME:foobar"}},
     "",
     kGlobalSSLMode},

    {"mongodb://user:pwd@[::1]/?authMechanism=GSSAPI&gssapiServiceName=foobar",
     "user",
     "pwd",
     kMaster,
     "",
     1,
     {{"authmechanism", "GSSAPI"}, {"gssapiServiceName", "foobar"}},
     "",
     kGlobalSSLMode},

    {"mongodb://%2Ftmp%2Fmongodb-27017.sock", "", "", kMaster, "", 1, {}, "", kGlobalSSLMode},

    {"mongodb://%2Ftmp%2Fmongodb-27017.sock,%2Ftmp%2Fmongodb-27018.sock/?replicaSet=replName",
     "",
     "",
     kSet,
     "replName",
     2,
     {{"replicaSet", "replName"}},
     "",
     kGlobalSSLMode},

    {"mongodb://localhost/?ssl=true", "", "", kMaster, "", 1, {{"ssl", "true"}}, "", kEnableSSL},
    {"mongodb://localhost/?ssl=false", "", "", kMaster, "", 1, {{"ssl", "false"}}, "", kDisableSSL},
    {"mongodb://localhost/?tls=true", "", "", kMaster, "", 1, {{"tls", "true"}}, "", kEnableSSL},
    {"mongodb://localhost/?tls=false", "", "", kMaster, "", 1, {{"tls", "false"}}, "", kDisableSSL},
};

const InvalidURITestCase invalidCases[] = {

    // No host.
    {"mongodb://"},
    {"mongodb://usr:pwd@/dbname?a=b"},

    // Username and password must be encoded (cannot have ':' or '@')
    {"mongodb://usr:pwd:@127.0.0.1/dbName?foo=a&c=b"},

    // Needs a "/" after the hosts and before the options.
    {"mongodb://localhost:27017,localhost:27018?replicaSet=missingSlash"},

    // Host list must actually be comma separated.
    {"mongodb://localhost:27017localhost:27018"},

    // % symbol in password must be escaped.
    {"mongodb://localhost:pass%word@127.0.0.1:27017", ErrorCodes::duplicateCodeForTest(51040)},

    // Domain sockets have to end in ".sock".
    {"mongodb://%2Fnotareal%2Fdomainsock"},

    // Database name cannot contain slash ("/"), backslash ("\"), space (" "), double-quote ("""),
    // or dollar sign ("$")
    {"mongodb://usr:pwd@localhost:27017/db$name?a=b"},
    {"mongodb://usr:pwd@localhost:27017/db/name?a=b"},
    {"mongodb://usr:pwd@localhost:27017/db\\name?a=b"},
    {"mongodb://usr:pwd@localhost:27017/db name?a=b"},
    {"mongodb://usr:pwd@localhost:27017/db\"name?a=b"},

    // Options must have a key
    {"mongodb://usr:pwd@localhost:27017/dbname?=b"},

    // Cannot skip a key value pair
    {"mongodb://usr:pwd@localhost:27017/dbname?a=b&&b=c"},

    // Multiple Unix domain sockets and auth DB resembling a socket (relative path)
    {"mongodb://rel%2Fmongodb-27017.sock,rel%2Fmongodb-27018.sock/admin.sock?replicaSet=replName"},

    // Multiple Unix domain sockets with auth DB resembling a path (relative path)
    {"mongodb://rel%2Fmongodb-27017.sock,rel%2Fmongodb-27018.sock/admin.shoe?replicaSet=replName"},

    // Multiple Unix domain sockets and auth DB resembling a socket (absolute path)
    {"mongodb://%2Ftmp%2Fmongodb-27017.sock,%2Ftmp%2Fmongodb-27018.sock/"
     "admin.sock?replicaSet=replName"},

    // Multiple Unix domain sockets with auth DB resembling a path (absolute path)
    {"mongodb://%2Ftmp%2Fmongodb-27017.sock,%2Ftmp%2Fmongodb-27018.sock/"
     "admin.shoe?replicaSet=replName"},

    // Missing value in key value pair for options
    {"mongodb://127.0.0.1:1234/dbName?foo=a&c=b&d"},
    {"mongodb://127.0.0.1:1234/dbName?foo=a&c=b&d="},
    {"mongodb://127.0.0.1:1234/dbName?foo=a&h=&c=b&d=6"},
    {"mongodb://127.0.0.1:1234/dbName?foo=a&h&c=b&d=6"},

    // Missing a hostname, or unparsable hostname(s)
    {"mongodb://,/dbName"},
    {"mongodb://user:pwd@,/dbName"},
    {"mongodb://localhost:1234:5678/dbName"},

    // Options can't have multiple question marks. Only one.
    {"mongodb://localhost:27017/?foo=a?c=b&d=e?asdf=foo"},

    // Missing a key in key value pair for options
    {"mongodb://127.0.0.1:1234/dbName?foo=a&=d&c=b"},

    // Missing an entire key-value pair
    {"mongodb://127.0.0.1:1234/dbName?foo=a&&c=b"},

    // Illegal value for ssl/tls.
    {"mongodb://127.0.0.1:1234/dbName?ssl=blah", ErrorCodes::duplicateCodeForTest(51041)},
    {"mongodb://127.0.0.1:1234/dbName?tls=blah", ErrorCodes::duplicateCodeForTest(51041)},
};

// Helper Method to take a filename for a json file and return the array of tests inside of it
BSONObj getBsonFromJsonFile(std::string fileName) {
    boost::filesystem::path directoryPath = boost::filesystem::current_path();
    boost::filesystem::path filePath(directoryPath / "src" / "mongo" / "client" /
                                     "mongo_uri_tests" / fileName);
    std::string filename(filePath.string());
    std::ifstream infile(filename.c_str());
    std::string data((std::istreambuf_iterator<char>(infile)), std::istreambuf_iterator<char>());
    BSONObj obj = fromjson(data);
    ASSERT_TRUE(obj.valid(BSONVersion::kLatest));
    ASSERT_TRUE(obj.hasField("tests"));
    BSONObj arr = obj.getField("tests").embeddedObject().getOwned();
    ASSERT_TRUE(arr.couldBeArray());
    return arr;
}

// Helper method to take a BSONElement and either extract its string or return an empty string
std::string returnStringFromElementOrNull(BSONElement element) {
    ASSERT_TRUE(!element.eoo());
    if (element.type() == jstNULL) {
        return std::string();
    }
    ASSERT_EQ(element.type(), String);
    return element.String();
}

// Helper method to take a valid test case, parse() it, and assure the output is correct
void testValidURIFormat(URITestCase testCase) {
    unittest::log() << "Testing URI: " << testCase.URI << '\n';
    std::string errMsg;
    const auto cs_status = MongoURI::parse(testCase.URI);
    ASSERT_OK(cs_status);
    auto result = cs_status.getValue();
    ASSERT_EQ(testCase.uname, result.getUser());
    ASSERT_EQ(testCase.password, result.getPassword());
    ASSERT_EQ(testCase.type, result.type());
    ASSERT_EQ(testCase.setname, result.getSetName());
    ASSERT_EQ(testCase.numservers, result.getServers().size());
    compareOptions(0, testCase.URI, result.getOptions(), testCase.options);
    ASSERT_EQ(testCase.database, result.getDatabase());
}

TEST(MongoURI, GoodTrickyURIs) {
    const size_t numCases = sizeof(validCases) / sizeof(validCases[0]);

    for (size_t i = 0; i != numCases; ++i) {
        const URITestCase testCase = validCases[i];
        testValidURIFormat(testCase);
    }
}

TEST(MongoURI, InvalidURIs) {
    const size_t numCases = sizeof(invalidCases) / sizeof(invalidCases[0]);

    for (size_t i = 0; i != numCases; ++i) {
        const InvalidURITestCase testCase = invalidCases[i];
        unittest::log() << "Testing URI: " << testCase.URI << '\n';
        auto cs_status = MongoURI::parse(testCase.URI);
        ASSERT_NOT_OK(cs_status);
        if (testCase.code) {
            ASSERT_EQUALS(*testCase.code, cs_status.getStatus());
        }
    }
}

TEST_F(ServiceContextTest, ValidButBadURIsFailToConnect) {
    // "invalid" is a TLD that cannot exit on the public internet (see rfc2606). It should always
    // parse as a valid URI, but connecting should always fail.
    auto sw_uri = MongoURI::parse("mongodb://user:pass@hostname.invalid:12345");
    ASSERT_OK(sw_uri.getStatus());
    auto uri = sw_uri.getValue();
    ASSERT_TRUE(uri.isValid());

    std::string errmsg;
    auto dbclient = uri.connect(StringData(), errmsg);
    ASSERT_EQ(dbclient, static_cast<decltype(dbclient)>(nullptr));
}

TEST(MongoURI, CloneURIForServer) {
    auto sw_uri = MongoURI::parse(
        "mongodb://localhost:27017,localhost:27018,localhost:27019/admin?replicaSet=rs1&ssl=true");
    ASSERT_OK(sw_uri.getStatus());

    auto uri = sw_uri.getValue();
    ASSERT_EQ(uri.type(), kSet);
    ASSERT_EQ(uri.getSetName(), "rs1");
    ASSERT_EQ(uri.getServers().size(), static_cast<std::size_t>(3));

    auto& uriOptions = uri.getOptions();
    ASSERT_EQ(uriOptions.at("ssl"), "true");

    auto clonedURI = uri.cloneURIForServer(HostAndPort{"localhost:27020"}, StringData());

    ASSERT_EQ(clonedURI.type(), kMaster);
    ASSERT_TRUE(clonedURI.getSetName().empty());
    ASSERT_EQ(clonedURI.getServers().size(), static_cast<std::size_t>(1));
    auto& clonedURIOptions = clonedURI.getOptions();
    ASSERT_EQ(clonedURIOptions.at("ssl"), "true");
}

/**
 * These tests come from the Mongo Uri Specifications for the drivers found at:
 * https://github.com/mongodb/specifications/tree/master/source/connection-string/tests
 * They have been altered as the Drivers specification is somewhat different from the shell
 * implementation.
 */
TEST(MongoURI, specTests) {
    const std::string files[] = {
        "mongo-uri-valid-auth.json",
        "mongo-uri-options.json",
        "mongo-uri-unix-sockets-absolute.json",
        "mongo-uri-unix-sockets-relative.json",
        "mongo-uri-warnings.json",
        "mongo-uri-host-identifiers.json",
        "mongo-uri-invalid.json",
    };

    for (const auto& file : files) {
        const auto testBson = getBsonFromJsonFile(file);

        for (const auto& testElement : testBson) {
            ASSERT_EQ(testElement.type(), Object);
            const auto test = testElement.Obj();

            // First extract the valid field and the uri field
            const auto validDoc = test.getField("valid");
            ASSERT_FALSE(validDoc.eoo());
            ASSERT_TRUE(validDoc.isBoolean());
            const auto valid = validDoc.Bool();

            const auto uriDoc = test.getField("uri");
            ASSERT_FALSE(uriDoc.eoo());
            ASSERT_EQ(uriDoc.type(), String);
            const auto uri = uriDoc.String();

            if (!valid) {
                // This uri string is invalid --> parse the uri and ensure it fails
                const InvalidURITestCase testCase = InvalidURITestCase{uri};
                unittest::log() << "Testing URI: " << testCase.URI << '\n';
                auto cs_status = MongoURI::parse(testCase.URI);
                ASSERT_NOT_OK(cs_status);
            } else {
                // This uri is valid -- > parse the remaining necessary fields

                // parse the auth options
                std::string database, username, password;

                const auto auth = test.getField("auth");
                ASSERT_FALSE(auth.eoo());
                if (auth.type() != jstNULL) {
                    ASSERT_EQ(auth.type(), Object);
                    const auto authObj = auth.embeddedObject();
                    database = returnStringFromElementOrNull(authObj.getField("db"));
                    username = returnStringFromElementOrNull(authObj.getField("username"));
                    password = returnStringFromElementOrNull(authObj.getField("password"));
                }

                // parse the hosts
                const auto hosts = test.getField("hosts");
                ASSERT_FALSE(hosts.eoo());
                ASSERT_EQ(hosts.type(), Array);
                const auto numHosts = static_cast<size_t>(hosts.Obj().nFields());

                // parse the options
                ConnectionString::ConnectionType connectionType = kMaster;
                size_t numOptions = 0;
                std::string setName;
                const auto optionsElement = test.getField("options");
                ASSERT_FALSE(optionsElement.eoo());
                MongoURI::OptionsMap options;
                if (optionsElement.type() != jstNULL) {
                    ASSERT_EQ(optionsElement.type(), Object);
                    const auto optionsObj = optionsElement.Obj();
                    numOptions = optionsObj.nFields();
                    const auto replsetElement = optionsObj.getField("replicaSet");
                    if (!replsetElement.eoo()) {
                        ASSERT_EQ(replsetElement.type(), String);
                        setName = replsetElement.String();
                        connectionType = kSet;
                    }

                    for (auto&& field : optionsElement.Obj()) {
                        if (field.type() == String) {
                            options[field.fieldNameStringData()] = field.String();
                        } else if (field.isNumber()) {
                            options[field.fieldNameStringData()] = std::to_string(field.Int());
                        } else {
                            MONGO_UNREACHABLE;
                        }
                    }
                }

                // Create the URITestCase abnd
                const URITestCase testCase = {
                    uri, username, password, connectionType, setName, numHosts, options, database};
                testValidURIFormat(testCase);
            }
        }
    }
}

TEST(MongoURI, srvRecordTest) {
    enum Expectation : bool { success = true, failure = false };
    const struct {
        int lineNumber;
        std::string uri;
        std::string user;
        std::string password;
        std::string database;
        std::vector<HostAndPort> hosts;
        std::map<MongoURI::CaseInsensitiveString, std::string> options;
        Expectation expectation;
    } tests[] = {
        // Test some non-SRV URIs to make sure that they do not perform expansions
        {__LINE__,
         "mongodb://test1.test.build.10gen.cc:12345/",
         "",
         "",
         "",
         {{"test1.test.build.10gen.cc", 12345}},
         {},
         success},

        {__LINE__,
         "mongodb://test6.test.build.10gen.cc:12345/",
         "",
         "",
         "",
         {{"test6.test.build.10gen.cc", 12345}},
         {},
         success},

        // Test a sample URI against each provided testing DNS entry
        {__LINE__,
         "mongodb+srv://test1.test.build.10gen.cc/",
         "",
         "",
         "",
         {{"localhost.test.build.10gen.cc", 27017}, {"localhost.test.build.10gen.cc", 27018}},
         {{"ssl", "true"}},
         success},

        // Test a sample URI against each provided testing DNS entry
        {__LINE__,
         "mongodb+srv://test1.test.build.10gen.cc/?ssl=false",
         "",
         "",
         "",
         {{"localhost.test.build.10gen.cc", 27017}, {"localhost.test.build.10gen.cc", 27018}},
         {{"ssl", "false"}},
         success},

        // Test a sample URI against the need for deep DNS relation
        {__LINE__,
         "mongodb+srv://test18.test.build.10gen.cc/?replicaSet=repl0",
         "",
         "",
         "",
         {
             {"localhost.sub.test.build.10gen.cc", 27017},
         },
         {
             {"ssl", "true"},
             {"replicaSet", "repl0"},
         },
         success},

        // Test a sample URI with FQDN against the need for deep DNS relation
        {__LINE__,
         "mongodb+srv://test18.test.build.10gen.cc./?replicaSet=repl0",
         "",
         "",
         "",
         {
             {"localhost.sub.test.build.10gen.cc", 27017},
         },
         {
             {"ssl", "true"},
             {"replicaSet", "repl0"},
         },
         success},

        {__LINE__,
         "mongodb+srv://user:password@test2.test.build.10gen.cc/"
         "database?someOption=someValue&someOtherOption=someOtherValue",
         "user",
         "password",
         "database",
         {{"localhost.test.build.10gen.cc", 27018}, {"localhost.test.build.10gen.cc", 27019}},
         {{"someOption", "someValue"}, {"someOtherOption", "someOtherValue"}, {"ssl", "true"}},
         success},


        {__LINE__,
         "mongodb+srv://user:password@test3.test.build.10gen.cc/"
         "database?someOption=someValue&someOtherOption=someOtherValue",
         "user",
         "password",
         "database",
         {{"localhost.test.build.10gen.cc", 27017}},
         {{"someOption", "someValue"}, {"someOtherOption", "someOtherValue"}, {"ssl", "true"}},
         success},


        {__LINE__,
         "mongodb+srv://user:password@test5.test.build.10gen.cc/"
         "database?someOption=someValue&someOtherOption=someOtherValue",
         "user",
         "password",
         "database",
         {{"localhost.test.build.10gen.cc", 27017}},
         {{"someOption", "someValue"},
          {"someOtherOption", "someOtherValue"},
          {"replicaSet", "repl0"},
          {"authSource", "thisDB"},
          {"ssl", "true"}},
         success},

        {__LINE__,
         "mongodb+srv://user:password@test5.test.build.10gen.cc/"
         "database?someOption=someValue&authSource=anotherDB&someOtherOption=someOtherValue",
         "user",
         "password",
         "database",
         {{"localhost.test.build.10gen.cc", 27017}},
         {{"someOption", "someValue"},
          {"someOtherOption", "someOtherValue"},
          {"replicaSet", "repl0"},
          {"replicaSet", "repl0"},
          {"authSource", "anotherDB"},
          {"ssl", "true"}},
         success},

        {__LINE__, "mongodb+srv://test6.test.build.10gen.cc/", "", "", "", {}, {}, failure},

        {__LINE__,
         "mongodb+srv://test6.test.build.10gen.cc/database",
         "",
         "",
         "database",
         {},
         {},
         failure},

        {__LINE__,
         "mongodb+srv://test6.test.build.10gen.cc/?authSource=anotherDB",
         "",
         "",
         "",
         {},
         {},
         failure},

        {__LINE__,
         "mongodb+srv://test6.test.build.10gen.cc/?irrelevantOption=irrelevantValue",
         "",
         "",
         "",
         {},
         {},
         failure},


        {__LINE__,
         "mongodb+srv://test6.test.build.10gen.cc/"
         "?irrelevantOption=irrelevantValue&authSource=anotherDB",
         "",
         "",
         "",
         {},
         {},
         failure},

        {__LINE__,
         "mongodb+srv://test7.test.build.10gen.cc./?irrelevantOption=irrelevantValue",
         "",
         "",
         "",
         {},
         {},
         failure},

        {__LINE__, "mongodb+srv://test7.test.build.10gen.cc./", "", "", "", {}, {}, failure},

        {__LINE__, "mongodb+srv://test8.test.build.10gen.cc./", "", "", "", {}, {}, failure},

        {__LINE__,
         "mongodb+srv://test10.test.build.10gen.cc./?irrelevantOption=irrelevantValue",
         "",
         "",
         "",
         {},
         {},
         failure},

        {__LINE__,
         "mongodb+srv://test11.test.build.10gen.cc./?irrelevantOption=irrelevantValue",
         "",
         "",
         "",
         {},
         {},
         failure},

        {__LINE__, "mongodb+srv://test12.test.build.10gen.cc./", "", "", "", {}, {}, failure},
        {__LINE__, "mongodb+srv://test13.test.build.10gen.cc./", "", "", "", {}, {}, failure},
        {__LINE__, "mongodb+srv://test14.test.build.10gen.cc./", "", "", "", {}, {}, failure},
        {__LINE__, "mongodb+srv://test15.test.build.10gen.cc./", "", "", "", {}, {}, failure},
        {__LINE__, "mongodb+srv://test16.test.build.10gen.cc./", "", "", "", {}, {}, failure},
        {__LINE__, "mongodb+srv://test17.test.build.10gen.cc./", "", "", "", {}, {}, failure},
        {__LINE__, "mongodb+srv://test19.test.build.10gen.cc./", "", "", "", {}, {}, failure},

        {__LINE__, "mongodb+srv://test12.test.build.10gen.cc/", "", "", "", {}, {}, failure},
        {__LINE__, "mongodb+srv://test13.test.build.10gen.cc/", "", "", "", {}, {}, failure},
        {__LINE__, "mongodb+srv://test14.test.build.10gen.cc/", "", "", "", {}, {}, failure},
        {__LINE__, "mongodb+srv://test15.test.build.10gen.cc/", "", "", "", {}, {}, failure},
        {__LINE__, "mongodb+srv://test16.test.build.10gen.cc/", "", "", "", {}, {}, failure},
        {__LINE__, "mongodb+srv://test17.test.build.10gen.cc/", "", "", "", {}, {}, failure},
        {__LINE__, "mongodb+srv://test19.test.build.10gen.cc/", "", "", "", {}, {}, failure},
    };

    for (const auto& test : tests) {
        auto rs = MongoURI::parse(test.uri);
        if (test.expectation == failure) {
            ASSERT_FALSE(rs.getStatus().isOK())
                << "Failing URI: " << test.uri << " data on line: " << test.lineNumber;
            continue;
        }
        ASSERT_OK(rs.getStatus()) << "Failed on URI: " << test.uri
                                  << " data on line: " << test.lineNumber;
        auto rv = rs.getValue();
        ASSERT_EQ(rv.getUser(), test.user)
            << "Failed on URI: " << test.uri << " data on line: " << test.lineNumber;
        ASSERT_EQ(rv.getPassword(), test.password)
            << "Failed on URI: " << test.uri << " data on line : " << test.lineNumber;
        ASSERT_EQ(rv.getDatabase(), test.database)
            << "Failed on URI: " << test.uri << " data on line : " << test.lineNumber;
        compareOptions(test.lineNumber, test.uri, rv.getOptions(), test.options);

        std::vector<HostAndPort> hosts(begin(rv.getServers()), end(rv.getServers()));
        std::sort(begin(hosts), end(hosts));
        auto expectedHosts = test.hosts;
        std::sort(begin(expectedHosts), end(expectedHosts));

        for (std::size_t i = 0; i < std::min(hosts.size(), expectedHosts.size()); ++i) {
            ASSERT_EQ(hosts[i], expectedHosts[i])
                << "Failed on URI: " << test.uri << " at host number" << i
                << " data on line: " << test.lineNumber;
        }
        ASSERT_TRUE(hosts.size() == expectedHosts.size())
            << "Failed on URI: " << test.uri << " Found " << hosts.size() << " hosts, expected "
            << expectedHosts.size() << " data on line: " << test.lineNumber;
    }
}

/*
 * Checks that redacting various secret info from URIs produces actually redacted URIs.
 * Also checks that SRV URI's don't turn into non-SRV URIs after redaction.
 */
TEST(MongoURI, Redact) {
    constexpr auto goodWithDBName = "mongodb://admin@localhost/admin"_sd;
    constexpr auto goodWithoutDBName = "mongodb://admin@localhost"_sd;
    constexpr auto goodWithOnlyDBAndHost = "mongodb://localhost/admin"_sd;
    const std::initializer_list<std::pair<StringData, StringData>> testCases = {
        {"mongodb://admin:password@localhost/admin"_sd, goodWithDBName},
        {"mongodb://admin@localhost/admin?secretConnectionOption=foo"_sd, goodWithDBName},
        {"mongodb://admin:password@localhost/admin?secretConnectionOptions"_sd, goodWithDBName},
        {"mongodb://admin@localhost/admin"_sd, goodWithDBName},
        {"mongodb://admin@localhost/admin?secretConnectionOptions", goodWithDBName},
        {"mongodb://admin:password@localhost"_sd, goodWithoutDBName},
        {"mongodb://admin@localhost", goodWithoutDBName},
        {"mongodb://localhost/admin?socketTimeoutMS=5", goodWithOnlyDBAndHost},
        {"mongodb://localhost/admin", goodWithOnlyDBAndHost},
    };

    for (const auto& testCase : testCases) {
        ASSERT_TRUE(MongoURI::isMongoURI(testCase.first));
        ASSERT_EQ(MongoURI::redact(testCase.first), testCase.second);
    }

    const auto toRedactSRV = "mongodb+srv://admin:password@localhost/admin?secret=foo"_sd;
    const auto redactedSRV = "mongodb+srv://admin@localhost/admin"_sd;
    ASSERT_EQ(MongoURI::redact(toRedactSRV), redactedSRV);
}

}  // namespace
}  // namespace mongo
