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


#include "mongo/client/mongo_uri.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bson_validate.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/db/auth/sasl_command_constants.h"
#include "mongo/db/database_name.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <cstddef>
#include <fstream>  // IWYU pragma: keep
#include <initializer_list>
#include <iostream>
#include <memory>
#include <string_view>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {
using namespace std::literals::string_view_literals;
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
#ifdef MONGO_CONFIG_GRPC
    bool gRPC = false;
#endif
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
                    std::string_view uri,
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
            LOGV2(20152,
                  "Option key-value pair doesn't equal expected pair",
                  "key"_attr = options[i].first.original(),
                  "value"_attr = options[i].second,
                  "expectedKey"_attr = expectedOptions[i].first.original(),
                  "expectedValue"_attr = expectedOptions[i].second,
                  "lineNumber"_attr = lineNumber);
            std::cerr << "Failing URI: \"" << uri << "\""
                      << " data on line: " << lineNumber << std::endl;
            ASSERT(false);
        }
    }
    ASSERT_EQ(options.size(), expectedOptions.size()) << "Failing URI: "
                                                      << " data on line: " << lineNumber << uri;
}

const ConnectionString::ConnectionType kMaster = ConnectionString::ConnectionType::kStandalone;
const ConnectionString::ConnectionType kReplicaSet = ConnectionString::ConnectionType::kReplicaSet;

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
     kReplicaSet,
     "replName",
     2,
     {{"a", "b"}, {"replicaSet", "replName"}},
     "dbname",
     kGlobalSSLMode},

    {"mongodb://needs%20encoding%25%23!%3C%3E:pwd@127.0.0.1,127.0.0.2/"
     "dbname?a=b&replicaSet=replName",
     "needs encoding%#!<>",
     "pwd",
     kReplicaSet,
     "replName",
     2,
     {{"a", "b"}, {"replicaSet", "replName"}},
     "dbname",
     kGlobalSSLMode},

    {"mongodb://needs%20encoding%25%23!%3C%3E:pwd@127.0.0.1,127.0.0.2/"
     "db@name?a=b&replicaSet=replName",
     "needs encoding%#!<>",
     "pwd",
     kReplicaSet,
     "replName",
     2,
     {{"a", "b"}, {"replicaSet", "replName"}},
     "db@name",
     kGlobalSSLMode},

    {"mongodb://user:needs%20encoding%25%23!%3C%3E@127.0.0.1,127.0.0.2/"
     "dbname?a=b&replicaSet=replName",
     "user",
     "needs encoding%#!<>",
     kReplicaSet,
     "replName",
     2,
     {{"a", "b"}, {"replicaSet", "replName"}},
     "dbname",
     kGlobalSSLMode},

    {"mongodb://user:pwd@127.0.0.1,127.0.0.2/dbname?a=b&replicaSet=needs%20encoding%25%23!%3C%3E",
     "user",
     "pwd",
     kReplicaSet,
     "needs encoding%#!<>",
     2,
     {{"a", "b"}, {"replicaSet", "needs encoding%#!<>"}},
     "dbname",
     kGlobalSSLMode},

    {"mongodb://user:pwd@127.0.0.1,127.0.0.2/needsencoding%40hello?a=b&replicaSet=replName",
     "user",
     "pwd",
     kReplicaSet,
     "replName",
     2,
     {{"a", "b"}, {"replicaSet", "replName"}},
     "needsencoding@hello",
     kGlobalSSLMode},

    {"mongodb://user:pwd@127.0.0.1,127.0.0.2/?replicaSet=replName",
     "user",
     "pwd",
     kReplicaSet,
     "replName",
     2,
     {{"replicaSet", "replName"}},
     "",
     kGlobalSSLMode},

    {"mongodb://user@127.0.0.1,127.0.0.2/?replicaSet=replName",
     "user",
     "",
     kReplicaSet,
     "replName",
     2,
     {{"replicaSet", "replName"}},
     "",
     kGlobalSSLMode},

    {"mongodb://user@127.0.0.1,127.0.0.2/?replicaset=replName",
     "user",
     "",
     kReplicaSet,
     "replName",
     2,
     {{"replicaSet", "replName"}},
     "",
     kGlobalSSLMode},

    {"mongodb://127.0.0.1,127.0.0.2/dbName?foo=a&c=b&replicaSet=replName",
     "",
     "",
     kReplicaSet,
     "replName",
     2,
     {{"foo", "a"}, {"c", "b"}, {"replicaSet", "replName"}},
     "dbName",
     kGlobalSSLMode},

    {"mongodb://user:pwd@127.0.0.1:1234,127.0.0.2:1234/?replicaSet=replName",
     "user",
     "pwd",
     kReplicaSet,
     "replName",
     2,
     {{"replicaSet", "replName"}},
     "",
     kGlobalSSLMode},

    {"mongodb://user@127.0.0.1:1234,127.0.0.2:1234/?replicaSet=replName",
     "user",
     "",
     kReplicaSet,
     "replName",
     2,
     {{"replicaSet", "replName"}},
     "",
     kGlobalSSLMode},

    {"mongodb://127.0.0.1:1234,127.0.0.1:1234/dbName?foo=a&c=b&replicaSet=replName",
     "",
     "",
     kReplicaSet,
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
     kReplicaSet,
     "replName",
     2,
     {{"replicaSet", "replName"}},
     "",
     kGlobalSSLMode},

    {"mongodb://user@[::1],127.0.0.2/?replicaSet=replName",
     "user",
     "",
     kReplicaSet,
     "replName",
     2,
     {{"replicaSet", "replName"}},
     "",
     kGlobalSSLMode},

    {"mongodb://[::1],127.0.0.2/dbName?foo=a&c=b&replicaSet=replName",
     "",
     "",
     kReplicaSet,
     "replName",
     2,
     {{"foo", "a"}, {"c", "b"}, {"replicaSet", "replName"}},
     "dbName",
     kGlobalSSLMode},

    {"mongodb://user:pwd@[::1]:1234,127.0.0.2:1234/?replicaSet=replName",
     "user",
     "pwd",
     kReplicaSet,
     "replName",
     2,
     {{"replicaSet", "replName"}},
     "",
     kGlobalSSLMode},

    {"mongodb://user@[::1]:1234,127.0.0.2:1234/?replicaSet=replName",
     "user",
     "",
     kReplicaSet,
     "replName",
     2,
     {{"replicaSet", "replName"}},
     "",
     kGlobalSSLMode},

    {"mongodb://[::1]:1234,[::1]:1234/dbName?foo=a&c=b&replicaSet=replName",
     "",
     "",
     kReplicaSet,
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
     kReplicaSet,
     "replName",
     2,
     {{"replicaSet", "replName"}},
     "",
     kGlobalSSLMode},

    {"mongodb://user@[::1],127.0.0.2/?replicaSet=replName",
     "user",
     "",
     kReplicaSet,
     "replName",
     2,
     {{"replicaSet", "replName"}},
     "",
     kGlobalSSLMode},

    {"mongodb://[::1],127.0.0.2/dbName?foo=a&c=b&replicaSet=replName",
     "",
     "",
     kReplicaSet,
     "replName",
     2,
     {{"foo", "a"}, {"c", "b"}, {"replicaSet", "replName"}},
     "dbName",
     kGlobalSSLMode},

    {"mongodb://user:pwd@[::1]:1234,127.0.0.2:1234/?replicaSet=replName",
     "user",
     "pwd",
     kReplicaSet,
     "replName",
     2,
     {{"replicaSet", "replName"}},
     "",
     kGlobalSSLMode},

    {"mongodb://user@[::1]:1234,127.0.0.2:1234/?replicaSet=replName",
     "user",
     "",
     kReplicaSet,
     "replName",
     2,
     {{"replicaSet", "replName"}},
     "",
     kGlobalSSLMode},

    {"mongodb://[::1]:1234,[::1]:1234/dbName?foo=a&c=b&replicaSet=replName",
     "",
     "",
     kReplicaSet,
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
     kReplicaSet,
     "replName",
     2,
     {{"replicaSet", "replName"}},
     "",
     kGlobalSSLMode},

    {"mongodb://localhost/?ssl=true", "", "", kMaster, "", 1, {{"ssl", "true"}}, "", kEnableSSL},
    {"mongodb://localhost/?ssl=false", "", "", kMaster, "", 1, {{"ssl", "false"}}, "", kDisableSSL},
    {"mongodb://localhost/?tls=true", "", "", kMaster, "", 1, {{"tls", "true"}}, "", kEnableSSL},
    {"mongodb://localhost/?tls=false", "", "", kMaster, "", 1, {{"tls", "false"}}, "", kDisableSSL},
#ifdef MONGO_CONFIG_GRPC
    {"mongodb://localhost", "", "", kMaster, "", 1, {}, "", kDisableSSL, false},
    {"mongodb://localhost/?grpc=false",
     "",
     "",
     kMaster,
     "",
     1,
     {{"grpc", "false"}},
     "",
     kGlobalSSLMode,
     false},
    {"mongodb://localhost/?grpc=true",
     "",
     "",
     kMaster,
     "",
     1,
     {{"grpc", "true"}},
     "",
     kGlobalSSLMode,
     true},
#endif
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
    {"mongodb://127.0.0.1:1234/dbName?ssl=blah", ErrorCodes::FailedToParse},
    {"mongodb://127.0.0.1:1234/dbName?tls=blah", ErrorCodes::FailedToParse},

#ifdef MONGO_CONFIG_GRPC
    {"mongodb://127.0.0.1:1234/dbName?gRPC=blah", ErrorCodes::FailedToParse},
#endif
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
    ASSERT_TRUE(validateBSON(obj).isOK());
    ASSERT_TRUE(obj.hasField("tests"));
    BSONObj arr = obj.getField("tests").embeddedObject().getOwned();
    ASSERT_TRUE(arr.couldBeArray());
    return arr;
}

// Helper method to take a BSONElement and either extract its string or return an empty string
std::string returnStringFromElementOrNull(BSONElement element) {
    ASSERT_TRUE(!element.eoo());
    if (element.type() == BSONType::null) {
        return std::string();
    }
    ASSERT_EQ(element.type(), BSONType::string);
    return element.String();
}

// Helper method to take a valid test case, parse() it, and assure the output is correct
void testValidURIFormat(URITestCase testCase) {
    LOGV2(20153, "Testing URI", "mongoUri"_attr = testCase.URI);
    const auto cs_status = MongoURI::parse(testCase.URI);
    ASSERT_OK(cs_status);
    auto result = cs_status.getValue();
    auto& cred = result.getCredential();
    ASSERT_EQ(testCase.uname, cred ? cred->username.value_or("") : "");
    ASSERT_EQ(testCase.password, cred ? cred->password.value_or("") : "");
    ASSERT_EQ(testCase.type, result.type());
    ASSERT_EQ(testCase.setname, result.getReplicaSetName());
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
        LOGV2(20154, "Testing URI", "mongoUri"_attr = testCase.URI);
        auto cs_status = MongoURI::parse(testCase.URI);
        ASSERT_NOT_OK(cs_status);
        if (testCase.code) {
            ASSERT_EQUALS(*testCase.code, cs_status.getStatus());
        }
    }
}

class URIConnectionTest : service_context_test::WithSetupTransportLayer,
                          public ServiceContextTest {};

TEST_F(URIConnectionTest, ValidButBadURIsFailToConnect) {
    // "invalid" is a TLD that cannot exit on the public internet (see rfc2606). It should always
    // parse as a valid URI, but connecting should always fail.
    auto sw_uri = MongoURI::parse("mongodb://user:pass@hostname.invalid:12345");
    ASSERT_OK(sw_uri.getStatus());
    auto uri = sw_uri.getValue();
    ASSERT_TRUE(uri.isValid());

    std::string errmsg;
    auto dbclient = uri.connect(std::string_view(), errmsg);
    ASSERT_EQ(dbclient, static_cast<decltype(dbclient)>(nullptr));
}

TEST(MongoURI, CloneURIForServer) {
    auto sw_uri = MongoURI::parse(
        "mongodb://localhost:27017,localhost:27018,localhost:27019/admin?replicaSet=rs1&ssl=true");
    ASSERT_OK(sw_uri.getStatus());

    auto uri = sw_uri.getValue();
    ASSERT_EQ(uri.type(), kReplicaSet);
    ASSERT_EQ(uri.getReplicaSetName(), "rs1");
    ASSERT_EQ(uri.getServers().size(), static_cast<std::size_t>(3));

    auto& uriOptions = uri.getOptions();
    ASSERT_EQ(uriOptions.at("ssl"), "true");

    auto clonedURI = uri.cloneURIForServer(HostAndPort{"localhost:27020"}, std::string_view());

    ASSERT_EQ(clonedURI.type(), kMaster);
    ASSERT_TRUE(clonedURI.getReplicaSetName().empty());
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
            ASSERT_EQ(testElement.type(), BSONType::object);
            const auto test = testElement.Obj();

            // First extract the valid field and the uri field
            const auto validDoc = test.getField("valid");
            ASSERT_FALSE(validDoc.eoo());
            ASSERT_TRUE(validDoc.isBoolean());
            const auto valid = validDoc.Bool();

            const auto uriDoc = test.getField("uri");
            ASSERT_FALSE(uriDoc.eoo());
            ASSERT_EQ(uriDoc.type(), BSONType::string);
            const auto uri = uriDoc.String();

            if (!valid) {
                // This uri string is invalid --> parse the uri and ensure it fails
                const InvalidURITestCase testCase = InvalidURITestCase{uri};
                LOGV2(20155, "Testing URI", "mongoUri"_attr = testCase.URI);
                auto cs_status = MongoURI::parse(testCase.URI);
                ASSERT_NOT_OK(cs_status);
            } else {
                // This uri is valid -- > parse the remaining necessary fields

                // parse the auth options
                std::string database, username, password;

                const auto auth = test.getField("auth");
                ASSERT_FALSE(auth.eoo());
                if (auth.type() != BSONType::null) {
                    ASSERT_EQ(auth.type(), BSONType::object);
                    const auto authObj = auth.embeddedObject();
                    database = returnStringFromElementOrNull(authObj.getField("db"));
                    username = returnStringFromElementOrNull(authObj.getField("username"));
                    password = returnStringFromElementOrNull(authObj.getField("password"));
                }

                // parse the hosts
                const auto hosts = test.getField("hosts");
                ASSERT_FALSE(hosts.eoo());
                ASSERT_EQ(hosts.type(), BSONType::array);
                const auto numHosts = static_cast<size_t>(hosts.Obj().nFields());

                // parse the options
                ConnectionString::ConnectionType connectionType = kMaster;
                std::string setName;
                const auto optionsElement = test.getField("options");
                ASSERT_FALSE(optionsElement.eoo());
                MongoURI::OptionsMap options;
                if (optionsElement.type() != BSONType::null) {
                    ASSERT_EQ(optionsElement.type(), BSONType::object);
                    const auto optionsObj = optionsElement.Obj();
                    const auto replsetElement = optionsObj.getField("replicaSet");
                    if (!replsetElement.eoo()) {
                        ASSERT_EQ(replsetElement.type(), BSONType::string);
                        setName = replsetElement.String();
                        connectionType = kReplicaSet;
                    }

                    for (auto&& field : optionsElement.Obj()) {
                        if (field.type() == BSONType::string) {
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
        auto& cred = rv.getCredential();
        ASSERT_EQ(cred ? cred->username.value_or("") : "", test.user)
            << "Failed on URI: " << test.uri << " data on line: " << test.lineNumber;
        ASSERT_EQ(cred ? cred->password.value_or("") : "", test.password)
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
    constexpr auto goodWithDBName = "mongodb://admin@localhost/admin"sv;
    constexpr auto goodWithoutDBName = "mongodb://admin@localhost"sv;
    constexpr auto goodWithOnlyDBAndHost = "mongodb://localhost/admin"sv;
    const std::initializer_list<std::pair<std::string_view, std::string_view>> testCases = {
        {"mongodb://admin:password@localhost/admin"sv, goodWithDBName},
        {"mongodb://admin@localhost/admin?secretConnectionOption=foo"sv, goodWithDBName},
        {"mongodb://admin:password@localhost/admin?secretConnectionOptions"sv, goodWithDBName},
        {"mongodb://admin@localhost/admin"sv, goodWithDBName},
        {"mongodb://admin@localhost/admin?secretConnectionOptions", goodWithDBName},
        {"mongodb://admin:password@localhost"sv, goodWithoutDBName},
        {"mongodb://admin@localhost", goodWithoutDBName},
        {"mongodb://localhost/admin?socketTimeoutMS=5", goodWithOnlyDBAndHost},
        {"mongodb://localhost/admin", goodWithOnlyDBAndHost},
    };

    for (const auto& testCase : testCases) {
        ASSERT_TRUE(MongoURI::isMongoURI(testCase.first));
        ASSERT_EQ(MongoURI::redact(testCase.first), testCase.second);
    }

    const auto toRedactSRV = "mongodb+srv://admin:password@localhost/admin?secret=foo"sv;
    const auto redactedSRV = "mongodb+srv://admin@localhost/admin"sv;
    ASSERT_EQ(MongoURI::redact(toRedactSRV), redactedSRV);
}

// MONGODB-CR is a deprecated mechanism. It must be recognized at URI parse time so that
// username and password are preserved in the credential (they are needed to produce a useful
// error at authentication time). Completely unknown mechanisms must fail at parse time.
TEST(MongoURI, DeprecatedAndUnknownMechanisms) {
    // MONGODB-CR: parse succeeds; username/password are preserved.
    {
        auto rs = MongoURI::parse("mongodb://user:pwd@localhost/db?authMechanism=MONGODB-CR");
        ASSERT_OK(rs.getStatus());
        const auto& cred = rs.getValue().getCredential();
        ASSERT_TRUE(cred.has_value());
        ASSERT_EQ(cred->mechanism, auth::AuthMechanism::kMongoDbCr);
        ASSERT_EQ(cred->username.value_or(""), "user");
        ASSERT_EQ(cred->password.value_or(""), "pwd");
        ASSERT_EQ(cred->db.value_or(""), "db");
    }

    // Truly unknown mechanism: parse must fail.
    {
        auto rs =
            MongoURI::parse("mongodb://user:pwd@localhost/db?authMechanism=COMPLETELY-UNKNOWN");
        ASSERT_NOT_OK(rs.getStatus());
    }
}

// External auth mechanisms (X.509, AWS, OIDC) must use "$external" as the auth db even when
// the URI carries a database path (e.g. /admin). Without an explicit authSource, picking up the
// URI path database breaks speculative authentication because the saslStart lands on "admin"
// instead of "$external".
TEST(MongoURI, ExternalMechanismDefaultsToExternalDb) {
    struct TestCase {
        std::string uri;
        auth::AuthMechanism expectedMech;
        std::string expectedDb;
    };

    const TestCase cases[] = {
        {.uri = "mongodb://localhost/admin?authMechanism=MONGODB-X509",
         .expectedMech = auth::AuthMechanism::kMongoX509,
         .expectedDb = "$external"},
        {.uri = "mongodb://localhost/?authMechanism=MONGODB-X509",
         .expectedMech = auth::AuthMechanism::kMongoX509,
         .expectedDb = "$external"},
        {.uri = "mongodb://localhost/admin?authMechanism=MONGODB-AWS",
         .expectedMech = auth::AuthMechanism::kMongoAWS,
         .expectedDb = "$external"},
        {.uri = "mongodb://localhost/admin?authMechanism=MONGODB-OIDC",
         .expectedMech = auth::AuthMechanism::kMongoOIDC,
         .expectedDb = "$external"},
        {.uri = "mongodb://user@localhost/admin?authMechanism=GSSAPI",
         .expectedMech = auth::AuthMechanism::kGSSAPI,
         .expectedDb = "$external"},
        // Explicit authSource overrides the external default.
        {.uri = "mongodb://localhost/admin?authMechanism=MONGODB-X509&authSource=$external",
         .expectedMech = auth::AuthMechanism::kMongoX509,
         .expectedDb = "$external"},
        {.uri = "mongodb://user:pwd@localhost/admin?authMechanism=SCRAM-SHA-256",
         .expectedMech = auth::AuthMechanism::kScramSha256,
         .expectedDb = "admin"},
        {.uri = "mongodb://user:pwd@localhost/admin?authMechanism=SCRAM-SHA-1",
         .expectedMech = auth::AuthMechanism::kScramSha1,
         .expectedDb = "admin"},
        {.uri = "mongodb://user:pwd@localhost/?authMechanism=PLAIN",
         .expectedMech = auth::AuthMechanism::kSaslPlain,
         .expectedDb = "$external"},
        {.uri = "mongodb://user:pwd@localhost/admin?authMechanism=MONGODB-CR",
         .expectedMech = auth::AuthMechanism::kMongoDbCr,
         .expectedDb = "admin"},
        // No authMechanism specified: URI parsing defaults to SCRAM-SHA-256.
        {.uri = "mongodb://user:pwd@localhost/admin",
         .expectedMech = auth::AuthMechanism::kScramSha256,
         .expectedDb = "admin"},
    };

    for (const auto& tc : cases) {
        auto rs = MongoURI::parse(tc.uri);
        ASSERT_OK(rs.getStatus()) << "URI: " << tc.uri;
        const auto& cred = rs.getValue().getCredential();
        ASSERT_TRUE(cred.has_value()) << "URI: " << tc.uri;
        ASSERT_EQ(cred->mechanism, tc.expectedMech) << "URI: " << tc.uri;
        ASSERT_EQ(cred->db.value_or(""), tc.expectedDb) << "URI: " << tc.uri;
    }
}

// Test authSource precedence according to the MongoDB driver specification:
// 1. Explicit authSource option (highest priority)
// 2. Database path in URI
// 3. Mechanism-specific default is used (e.g. "admin" for SCRAM).
TEST(MongoURI, AuthSourcePrecedence) {
    struct TestCase {
        std::string uri;
        std::string expectedDb;
        std::string description;
    };

    const TestCase cases[] = {
        // Explicit authSource takes precedence over database path
        {"mongodb://user:pwd@localhost/mydb?authSource=authdb",
         "authdb",
         "Explicit authSource overrides database path"},

        // Database path is used when no authSource specified
        {"mongodb://user:pwd@localhost/mydb", "mydb", "Database path used as authSource"},

        // Empty database path with explicit authSource
        {"mongodb://user:pwd@localhost/?authSource=customdb",
         "customdb",
         "Explicit authSource with no database path"},

        // No database path and no authSource uses mechanism-specific default.
        {"mongodb://user:pwd@localhost/?authMechanism=SCRAM-SHA-256",
         "admin",
         "No authSource and no database path default to admin for SCRAM"},

        // Explicit authSource can override to non-standard database
        {"mongodb://user:pwd@localhost/prod?authSource=test",
         "test",
         "authSource overrides to different database"},

        // Database path with special characters (URL encoded)
        {"mongodb://user:pwd@localhost/my%40db?authMechanism=SCRAM-SHA-256",
         "my@db",
         "Database path with special characters"},

        // Explicit authSource with special characters
        {"mongodb://user:pwd@localhost/?authSource=my%40db",
         "my@db",
         "authSource with special characters"},
    };

    for (const auto& tc : cases) {
        auto rs = MongoURI::parse(tc.uri);
        ASSERT_OK(rs.getStatus()) << "Failed to parse URI: " << tc.uri
                                  << " (test: " << tc.description << ")";
        const auto& cred = rs.getValue().getCredential();
        ASSERT_TRUE(cred.has_value())
            << "No credential created for URI: " << tc.uri << " (test: " << tc.description << ")";
        ASSERT_TRUE(cred->db.has_value()) << "No authSource in credential for URI: " << tc.uri
                                          << " (test: " << tc.description << ")";
        ASSERT_EQ(*cred->db, tc.expectedDb)
            << "Wrong authSource for URI: " << tc.uri << " (test: " << tc.description << ")";
    }
}

// For non-external mechanisms at URI parse time:
// 1. Database path is used when present.
// 2. Explicit authSource overrides the database path.
// 3. If both are absent, mechanism-specific default is used.
TEST(MongoURI, NonExternalMechanismAuthSourceDefaults) {
    struct TestCase {
        std::string uri;
        std::string expectedDb;
        std::string description;
    };

    const TestCase cases[] = {
        // SCRAM-SHA-256 with database path
        {"mongodb://user:pwd@localhost/mydb?authMechanism=SCRAM-SHA-256",
         "mydb",
         "SCRAM-SHA-256 uses database path"},

        // SCRAM-SHA-1 with database path
        {"mongodb://user:pwd@localhost/testdb?authMechanism=SCRAM-SHA-1",
         "testdb",
         "SCRAM-SHA-1 uses database path"},

        // PLAIN with explicit authSource (PLAIN typically requires $external)
        {"mongodb://user:pwd@localhost/?authMechanism=PLAIN&authSource=$external",
         "$external",
         "PLAIN with explicit $external authSource"},

        // PLAIN with database path defaults to that database.
        {"mongodb://user:pwd@localhost/mydb?authMechanism=PLAIN",
         "mydb",
         "PLAIN with database path defaults to that database"},

        // SCRAM with no database path and no authSource defaults to admin.
        {"mongodb://user:pwd@localhost/?authMechanism=SCRAM-SHA-256",
         "admin",
         "SCRAM-SHA-256 without authSource or database path defaults to admin"},

        // PLAIN with no database path and no authSource defaults to $external.
        {"mongodb://user:pwd@localhost/?authMechanism=PLAIN",
         "$external",
         "PLAIN without authSource or database path defaults to $external"},
    };

    for (const auto& tc : cases) {
        auto rs = MongoURI::parse(tc.uri);
        ASSERT_OK(rs.getStatus()) << "Failed to parse URI: " << tc.uri
                                  << " (test: " << tc.description << ")";
        const auto& cred = rs.getValue().getCredential();
        ASSERT_TRUE(cred.has_value())
            << "No credential created for URI: " << tc.uri << " (test: " << tc.description << ")";
        ASSERT_TRUE(cred->db.has_value()) << "No authSource in credential for URI: " << tc.uri
                                          << " (test: " << tc.description << ")";
        ASSERT_EQ(*cred->db, tc.expectedDb)
            << "Wrong authSource for URI: " << tc.uri << " (test: " << tc.description << ")";
    }
}

// Validate authSource defaulting in MongoURI::makeAuthObjFromOptions so auth command generation
// matches parse-time defaults.
TEST(MongoURI, MakeAuthObjAuthSourceDefaults) {
    struct TestCase {
        std::string uri;
        std::vector<std::string> saslMechsForAuth;
        std::string expectedDb;
        std::string description;
    };

    const TestCase cases[] = {
        {"mongodb://user:pwd@localhost/mydb?authSource=authdb&authMechanism=SCRAM-SHA-256",
         {},
         "authdb",
         "Explicit authSource overrides database path"},
        {"mongodb://localhost/admin?authMechanism=MONGODB-AWS",
         {},
         "$external",
         "External mechanism defaults to $external"},
        {"mongodb://user:pwd@localhost/mydb?authMechanism=PLAIN",
         {},
         "mydb",
         "PLAIN defaults to database path when present"},
        {"mongodb://user:pwd@localhost/?authMechanism=PLAIN",
         {},
         "$external",
         "PLAIN defaults to $external when database path is absent"},
        {"mongodb://user:pwd@localhost/mydb?authMechanism=SCRAM-SHA-256",
         {},
         "mydb",
         "SCRAM defaults to database path when present"},
        {"mongodb://user:pwd@localhost/?authMechanism=SCRAM-SHA-256",
         {},
         "admin",
         "SCRAM defaults to admin when database path is absent"},
        {"mongodb://user:pwd@localhost/",
         {std::string{auth::kMechanismScramSha1}},
         "admin",
         "Negotiated SCRAM-SHA-1 defaults to admin when database path is absent"},
    };

    for (const auto& tc : cases) {
        auto rs = MongoURI::parse(tc.uri);
        ASSERT_OK(rs.getStatus()) << "Failed to parse URI: " << tc.uri
                                  << " (test: " << tc.description << ")";

        auto authObj =
            rs.getValue().makeAuthObjFromOptions(/*maxWireVersion*/ 0, tc.saslMechsForAuth);
        ASSERT_TRUE(authObj.has_value())
            << "No auth object created for URI: " << tc.uri << " (test: " << tc.description << ")";

        const auto sourceField = authObj->getField(saslCommandUserDBFieldName);
        ASSERT_FALSE(sourceField.eoo()) << "No auth db in auth object for URI: " << tc.uri
                                        << " (test: " << tc.description << ")";
        ASSERT_EQ(sourceField.type(), BSONType::string)
            << "Auth db has wrong type for URI: " << tc.uri << " (test: " << tc.description << ")";
        ASSERT_EQ(sourceField.String(), tc.expectedDb)
            << "Wrong auth db in auth object for URI: " << tc.uri << " (test: " << tc.description
            << ")";
    }
}

// Test that explicit authSource can override the external mechanism default
TEST(MongoURI, ExplicitAuthSourceOverridesExternalDefault) {
    struct TestCase {
        std::string uri;
        std::string expectedDb;
        std::string description;
    };

    const TestCase cases[] = {
        // X.509 with explicit non-$external authSource (unusual but should be allowed)
        {"mongodb://CN=test@localhost/?authMechanism=MONGODB-X509&authSource=admin",
         "admin",
         "X.509 with explicit admin authSource"},

        // AWS with explicit authSource
        {"mongodb://localhost/?authMechanism=MONGODB-AWS&authSource=custom",
         "custom",
         "MONGODB-AWS with explicit custom authSource"},

        // OIDC with explicit authSource
        {"mongodb://localhost/?authMechanism=MONGODB-OIDC&authSource=mydb",
         "mydb",
         "MONGODB-OIDC with explicit custom authSource"},

        // GSSAPI with explicit authSource
        {"mongodb://user@localhost/?authMechanism=GSSAPI&authSource=test",
         "test",
         "GSSAPI with explicit test authSource"},
    };

    for (const auto& tc : cases) {
        auto rs = MongoURI::parse(tc.uri);
        ASSERT_OK(rs.getStatus()) << "Failed to parse URI: " << tc.uri
                                  << " (test: " << tc.description << ")";
        const auto& cred = rs.getValue().getCredential();
        ASSERT_TRUE(cred.has_value())
            << "No credential created for URI: " << tc.uri << " (test: " << tc.description << ")";
        ASSERT_TRUE(cred->db.has_value()) << "No authSource in credential for URI: " << tc.uri
                                          << " (test: " << tc.description << ")";
        ASSERT_EQ(*cred->db, tc.expectedDb)
            << "Wrong authSource for URI: " << tc.uri << " (test: " << tc.description << ")";
    }
}

// Test URIs without credentials don't create credentials
TEST(MongoURI, NoCredentialsWithoutAuthInfo) {
    struct TestCase {
        std::string uri;
        std::string description;
    };

    const TestCase cases[] = {
        {"mongodb://localhost/", "No username, no database"},
        {"mongodb://localhost/mydb", "Database path but no username"},
        {"mongodb://localhost/?authSource=test", "authSource but no username"},
    };

    for (const auto& tc : cases) {
        auto rs = MongoURI::parse(tc.uri);
        ASSERT_OK(rs.getStatus()) << "Failed to parse URI: " << tc.uri
                                  << " (test: " << tc.description << ")";
        const auto& cred = rs.getValue().getCredential();
        ASSERT_FALSE(cred.has_value()) << "Unexpected credential created for URI: " << tc.uri
                                       << " (test: " << tc.description << ")";
    }
}

TEST(MongoURI, MakeAuthObjMechanismNegotiation) {
    struct TestCase {
        std::string uri;
        std::vector<std::string> saslMechsForAuth;
        std::string expectedMechanism;
        std::string description;
    };

    const std::string base = "mongodb://user:pwd@localhost/";
    const TestCase cases[] = {
        {.uri = base,
         .saslMechsForAuth = {std::string{auth::kMechanismScramSha256},
                              std::string{auth::kMechanismScramSha1}},
         .expectedMechanism = std::string{auth::kMechanismScramSha256},
         .description = "SHA-256 preferred when both advertised"},
        {.uri = base,
         .saslMechsForAuth = {std::string{auth::kMechanismScramSha1}},
         .expectedMechanism = std::string{auth::kMechanismScramSha1},
         .description = "SHA-1 chosen when only SHA-1 advertised"},
        {.uri = base,
         .saslMechsForAuth = {std::string{auth::kMechanismScramSha256}},
         .expectedMechanism = std::string{auth::kMechanismScramSha256},
         .description = "SHA-256 chosen when only SHA-256 advertised"},
        {.uri = base,
         .saslMechsForAuth = {},
         .expectedMechanism = std::string{auth::kMechanismScramSha1},
         .description = "SHA-1 chosen when server advertises no mechanisms"},
        {.uri = base + "?authMechanism=SCRAM-SHA-256",
         .saslMechsForAuth = {std::string{auth::kMechanismScramSha1}},
         .expectedMechanism = std::string{auth::kMechanismScramSha256},
         .description = "Explicit URI mechanism overrides saslMechsForAuth"},
    };

    for (const auto& tc : cases) {
        auto rs = MongoURI::parse(tc.uri);
        ASSERT_OK(rs.getStatus()) << tc.description;

        auto authObj = rs.getValue().makeAuthObjFromOptions(0, tc.saslMechsForAuth);
        ASSERT_TRUE(authObj.has_value()) << tc.description;

        const auto mechField = authObj->getField(saslCommandMechanismFieldName);
        ASSERT_FALSE(mechField.eoo()) << tc.description;
        ASSERT_EQ(mechField.String(), tc.expectedMechanism) << tc.description;
    }
}

TEST(MongoURI, EmptyAuthSourceRejectedByParser) {
    // The URI parser rejects authSource= (empty value); it does not silently ignore it.
    const std::string uri = "mongodb://user:pwd@localhost/?authSource=&authMechanism=SCRAM-SHA-256";
    auto rs = MongoURI::parse(uri);
    ASSERT_NOT_OK(rs.getStatus());
    ASSERT_EQ(rs.getStatus().code(), ErrorCodes::FailedToParse);
}

TEST(MongoURI, MissingAuthSourceDefaultsToAdmin) {
    // SCRAM-SHA-256 with no authSource specified should default the auth database to "admin".
    const std::string uri = "mongodb://user:pwd@localhost/?authMechanism=SCRAM-SHA-256";
    auto rs = MongoURI::parse(uri);
    ASSERT_OK(rs.getStatus());

    auto authObj = rs.getValue().makeAuthObjFromOptions(0, {});
    ASSERT_TRUE(authObj.has_value());

    const auto sourceField = authObj->getField(saslCommandUserDBFieldName);
    ASSERT_FALSE(sourceField.eoo());
    ASSERT_EQ(sourceField.String(), DatabaseName::kAdmin.db(omitTenant));
}

}  // namespace
}  // namespace mongo
