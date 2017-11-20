/**
 *    Copyright (C) 2009-2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
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

#include <fstream>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/client/mongo_uri.h"
#include "mongo/unittest/unittest.h"

#include <boost/filesystem/operations.hpp>

namespace {
using mongo::MongoURI;

struct URITestCase {
    std::string URI;
    std::string uname;
    std::string password;
    mongo::ConnectionString::ConnectionType type;
    std::string setname;
    size_t numservers;
    size_t numOptions;
    std::string database;
};

struct InvalidURITestCase {
    std::string URI;
};

const mongo::ConnectionString::ConnectionType kMaster = mongo::ConnectionString::MASTER;
const mongo::ConnectionString::ConnectionType kSet = mongo::ConnectionString::SET;

const URITestCase validCases[] = {

    {"mongodb://user:pwd@127.0.0.1", "user", "pwd", kMaster, "", 1, 0, ""},

    {"mongodb://user@127.0.0.1", "user", "", kMaster, "", 1, 0, ""},

    {"mongodb://localhost/?foo=bar", "", "", kMaster, "", 1, 1, ""},

    {"mongodb://localhost,/?foo=bar", "", "", kMaster, "", 1, 1, ""},

    {"mongodb://user:pwd@127.0.0.1:1234", "user", "pwd", kMaster, "", 1, 0, ""},

    {"mongodb://user@127.0.0.1:1234", "user", "", kMaster, "", 1, 0, ""},

    {"mongodb://127.0.0.1:1234/dbName?foo=a&c=b", "", "", kMaster, "", 1, 2, "dbName"},

    {"mongodb://127.0.0.1/dbName?foo=a&c=b", "", "", kMaster, "", 1, 2, "dbName"},

    {"mongodb://user:pwd@127.0.0.1,/dbName?foo=a&c=b", "user", "pwd", kMaster, "", 1, 2, "dbName"},

    {"mongodb://user:pwd@127.0.0.1,127.0.0.2/dbname?a=b&replicaSet=replName",
     "user",
     "pwd",
     kSet,
     "replName",
     2,
     2,
     "dbname"},

    {"mongodb://needs%20encoding%25%23!%3C%3E:pwd@127.0.0.1,127.0.0.2/"
     "dbname?a=b&replicaSet=replName",
     "needs encoding%#!<>",
     "pwd",
     kSet,
     "replName",
     2,
     2,
     "dbname"},

    {"mongodb://needs%20encoding%25%23!%3C%3E:pwd@127.0.0.1,127.0.0.2/"
     "db@name?a=b&replicaSet=replName",
     "needs encoding%#!<>",
     "pwd",
     kSet,
     "replName",
     2,
     2,
     "db@name"},

    {"mongodb://user:needs%20encoding%25%23!%3C%3E@127.0.0.1,127.0.0.2/"
     "dbname?a=b&replicaSet=replName",
     "user",
     "needs encoding%#!<>",
     kSet,
     "replName",
     2,
     2,
     "dbname"},

    {"mongodb://user:pwd@127.0.0.1,127.0.0.2/dbname?a=b&replicaSet=needs%20encoding%25%23!%3C%3E",
     "user",
     "pwd",
     kSet,
     "needs encoding%#!<>",
     2,
     2,
     "dbname"},

    {"mongodb://user:pwd@127.0.0.1,127.0.0.2/needsencoding%40hello?a=b&replicaSet=replName",
     "user",
     "pwd",
     kSet,
     "replName",
     2,
     2,
     "needsencoding@hello"},

    {"mongodb://user:pwd@127.0.0.1,127.0.0.2/?replicaSet=replName",
     "user",
     "pwd",
     kSet,
     "replName",
     2,
     1,
     ""},

    {"mongodb://user@127.0.0.1,127.0.0.2/?replicaSet=replName",
     "user",
     "",
     kSet,
     "replName",
     2,
     1,
     ""},

    {"mongodb://127.0.0.1,127.0.0.2/dbName?foo=a&c=b&replicaSet=replName",
     "",
     "",
     kSet,
     "replName",
     2,
     3,
     "dbName"},

    {"mongodb://user:pwd@127.0.0.1:1234,127.0.0.2:1234/?replicaSet=replName",
     "user",
     "pwd",
     kSet,
     "replName",
     2,
     1,
     ""},

    {"mongodb://user@127.0.0.1:1234,127.0.0.2:1234/?replicaSet=replName",
     "user",
     "",
     kSet,
     "replName",
     2,
     1,
     ""},

    {"mongodb://127.0.0.1:1234,127.0.0.1:1234/dbName?foo=a&c=b&replicaSet=replName",
     "",
     "",
     kSet,
     "replName",
     2,
     3,
     "dbName"},

    {"mongodb://user:pwd@[::1]", "user", "pwd", kMaster, "", 1, 0, ""},

    {"mongodb://user@[::1]", "user", "", kMaster, "", 1, 0, ""},

    {"mongodb://[::1]/dbName?foo=a&c=b", "", "", kMaster, "", 1, 2, "dbName"},

    {"mongodb://user:pwd@[::1]:1234", "user", "pwd", kMaster, "", 1, 0, ""},

    {"mongodb://user@[::1]:1234", "user", "", kMaster, "", 1, 0, ""},

    {"mongodb://[::1]:1234/dbName?foo=a&c=b", "", "", kMaster, "", 1, 2, "dbName"},

    {"mongodb://user:pwd@[::1],127.0.0.2/?replicaSet=replName",
     "user",
     "pwd",
     kSet,
     "replName",
     2,
     1,
     ""},

    {"mongodb://user@[::1],127.0.0.2/?replicaSet=replName", "user", "", kSet, "replName", 2, 1, ""},

    {"mongodb://[::1],127.0.0.2/dbName?foo=a&c=b&replicaSet=replName",
     "",
     "",
     kSet,
     "replName",
     2,
     3,
     "dbName"},

    {"mongodb://user:pwd@[::1]:1234,127.0.0.2:1234/?replicaSet=replName",
     "user",
     "pwd",
     kSet,
     "replName",
     2,
     1,
     ""},

    {"mongodb://user@[::1]:1234,127.0.0.2:1234/?replicaSet=replName",
     "user",
     "",
     kSet,
     "replName",
     2,
     1,
     ""},

    {"mongodb://[::1]:1234,[::1]:1234/dbName?foo=a&c=b&replicaSet=replName",
     "",
     "",
     kSet,
     "replName",
     2,
     3,
     "dbName"},

    {"mongodb://user:pwd@[::1]", "user", "pwd", kMaster, "", 1, 0, ""},

    {"mongodb://user@[::1]", "user", "", kMaster, "", 1, 0, ""},

    {"mongodb://[::1]/dbName?foo=a&c=b", "", "", kMaster, "", 1, 2, "dbName"},

    {"mongodb://user:pwd@[::1]:1234", "user", "pwd", kMaster, "", 1, 0, ""},

    {"mongodb://user@[::1]:1234", "user", "", kMaster, "", 1, 0, ""},

    {"mongodb://[::1]:1234/dbName?foo=a&c=b", "", "", kMaster, "", 1, 2, "dbName"},

    {"mongodb://user:pwd@[::1],127.0.0.2/?replicaSet=replName",
     "user",
     "pwd",
     kSet,
     "replName",
     2,
     1,
     ""},

    {"mongodb://user@[::1],127.0.0.2/?replicaSet=replName", "user", "", kSet, "replName", 2, 1, ""},

    {"mongodb://[::1],127.0.0.2/dbName?foo=a&c=b&replicaSet=replName",
     "",
     "",
     kSet,
     "replName",
     2,
     3,
     "dbName"},

    {"mongodb://user:pwd@[::1]:1234,127.0.0.2:1234/?replicaSet=replName",
     "user",
     "pwd",
     kSet,
     "replName",
     2,
     1,
     ""},

    {"mongodb://user@[::1]:1234,127.0.0.2:1234/?replicaSet=replName",
     "user",
     "",
     kSet,
     "replName",
     2,
     1,
     ""},

    {"mongodb://[::1]:1234,[::1]:1234/dbName?foo=a&c=b&replicaSet=replName",
     "",
     "",
     kSet,
     "replName",
     2,
     3,
     "dbName"},

    {"mongodb://user:pwd@[::1]/?authMechanism=GSSAPI&authMechanismProperties=SERVICE_NAME:foobar",
     "user",
     "pwd",
     kMaster,
     "",
     1,
     2,
     ""},

    {"mongodb://user:pwd@[::1]/?authMechanism=GSSAPI&gssapiServiceName=foobar",
     "user",
     "pwd",
     kMaster,
     "",
     1,
     2,
     ""},

    {"mongodb://%2Ftmp%2Fmongodb-27017.sock", "", "", kMaster, "", 1, 0, ""},

    {"mongodb://%2Ftmp%2Fmongodb-27017.sock,%2Ftmp%2Fmongodb-27018.sock/?replicaSet=replName",
     "",
     "",
     kSet,
     "replName",
     2,
     1,
     ""},
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
};

// Helper Method to take a filename for a json file and return the array of tests inside of it
mongo::BSONObj getBsonFromJsonFile(std::string fileName) {
    boost::filesystem::path directoryPath = boost::filesystem::current_path();
    boost::filesystem::path filePath(directoryPath / "src" / "mongo" / "client" /
                                     "mongo_uri_tests" / fileName);
    std::string filename(filePath.string());
    std::ifstream infile(filename.c_str());
    std::string data((std::istreambuf_iterator<char>(infile)), std::istreambuf_iterator<char>());
    mongo::BSONObj obj = mongo::fromjson(data);
    ASSERT_TRUE(obj.valid(mongo::BSONVersion::kLatest));
    ASSERT_TRUE(obj.hasField("tests"));
    mongo::BSONObj arr = obj.getField("tests").embeddedObject().getOwned();
    ASSERT_TRUE(arr.couldBeArray());
    return arr;
}

// Helper method to take a BSONElement and either extract its string or return an empty string
std::string returnStringFromElementOrNull(mongo::BSONElement element) {
    ASSERT_TRUE(!element.eoo());
    if (element.type() == mongo::jstNULL) {
        return std::string();
    }
    ASSERT_EQ(element.type(), mongo::String);
    return element.String();
}

// Helper method to take a valid test case, parse() it, and assure the output is correct
void testValidURIFormat(URITestCase testCase) {
    mongo::unittest::log() << "Testing URI: " << testCase.URI << '\n';
    std::string errMsg;
    const auto cs_status = MongoURI::parse(testCase.URI);
    ASSERT_OK(cs_status);
    auto result = cs_status.getValue();
    ASSERT_EQ(testCase.uname, result.getUser());
    ASSERT_EQ(testCase.password, result.getPassword());
    ASSERT_EQ(testCase.type, result.type());
    ASSERT_EQ(testCase.setname, result.getSetName());
    ASSERT_EQ(testCase.numservers, result.getServers().size());
    ASSERT_EQ(testCase.numOptions, result.getOptions().size());
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
        mongo::unittest::log() << "Testing URI: " << testCase.URI << '\n';
        auto cs_status = MongoURI::parse(testCase.URI);
        ASSERT_NOT_OK(cs_status);
    }
}

TEST(MongoURI, ValidButBadURIsFailToConnect) {
    // "invalid" is a TLD that cannot exit on the public internet (see rfc2606). It should always
    // parse as a valid URI, but connecting should always fail.
    auto sw_uri = MongoURI::parse("mongodb://user:pass@hostname.invalid:12345");
    ASSERT_OK(sw_uri.getStatus());
    auto uri = sw_uri.getValue();
    ASSERT_TRUE(uri.isValid());

    std::string errmsg;
    auto dbclient = uri.connect(mongo::StringData(), errmsg);
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

    auto clonedURI = uri.cloneURIForServer(mongo::HostAndPort{"localhost:27020"});

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
            ASSERT_EQ(testElement.type(), mongo::Object);
            const auto test = testElement.Obj();

            // First extract the valid field and the uri field
            const auto validDoc = test.getField("valid");
            ASSERT_FALSE(validDoc.eoo());
            ASSERT_TRUE(validDoc.isBoolean());
            const auto valid = validDoc.Bool();

            const auto uriDoc = test.getField("uri");
            ASSERT_FALSE(uriDoc.eoo());
            ASSERT_EQ(uriDoc.type(), mongo::String);
            const auto uri = uriDoc.String();

            if (!valid) {
                // This uri string is invalid --> parse the uri and ensure it fails
                const InvalidURITestCase testCase = {uri};
                mongo::unittest::log() << "Testing URI: " << testCase.URI << '\n';
                auto cs_status = MongoURI::parse(testCase.URI);
                ASSERT_NOT_OK(cs_status);
            } else {
                // This uri is valid -- > parse the remaining necessary fields

                // parse the auth options
                std::string database, username, password;

                const auto auth = test.getField("auth");
                ASSERT_FALSE(auth.eoo());
                if (auth.type() != mongo::jstNULL) {
                    ASSERT_EQ(auth.type(), mongo::Object);
                    const auto authObj = auth.embeddedObject();
                    database = returnStringFromElementOrNull(authObj.getField("db"));
                    username = returnStringFromElementOrNull(authObj.getField("username"));
                    password = returnStringFromElementOrNull(authObj.getField("password"));
                }

                // parse the hosts
                const auto hosts = test.getField("hosts");
                ASSERT_FALSE(hosts.eoo());
                ASSERT_EQ(hosts.type(), mongo::Array);
                const auto numHosts = static_cast<size_t>(hosts.Obj().nFields());

                // parse the options
                mongo::ConnectionString::ConnectionType connectionType = kMaster;
                size_t numOptions = 0;
                std::string setName;
                const auto optionsElement = test.getField("options");
                ASSERT_FALSE(optionsElement.eoo());
                if (optionsElement.type() != mongo::jstNULL) {
                    ASSERT_EQ(optionsElement.type(), mongo::Object);
                    const auto optionsObj = optionsElement.Obj();
                    numOptions = optionsObj.nFields();
                    const auto replsetElement = optionsObj.getField("replicaSet");
                    if (!replsetElement.eoo()) {
                        ASSERT_EQ(replsetElement.type(), mongo::String);
                        setName = replsetElement.String();
                        connectionType = kSet;
                    }
                }

                // Create the URITestCase abnd
                const URITestCase testCase = {uri,
                                              username,
                                              password,
                                              connectionType,
                                              setName,
                                              numHosts,
                                              numOptions,
                                              database};
                testValidURIFormat(testCase);
            }
        }
    }
}

TEST(MongoURI, srvRecordTest) {
    using namespace mongo;
    const struct {
        std::string uri;
        std::string user;
        std::string password;
        std::string database;
        std::vector<HostAndPort> hosts;
        std::map<std::string, std::string> options;
    } tests[] = {
        // Test some non-SRV URIs to make sure that they do not perform expansions
        {"mongodb://test1.test.build.10gen.cc:12345/",
         "",
         "",
         "",
         {{"test1.test.build.10gen.cc", 12345}},
         {}},
        {"mongodb://test6.test.build.10gen.cc:12345/",
         "",
         "",
         "",
         {{"test6.test.build.10gen.cc", 12345}},
         {}},

        // Test a sample URI against each provided testing DNS entry
        {"mongodb+srv://test1.test.build.10gen.cc/",
         "",
         "",
         "",
         {{"localhost.test.build.10gen.cc.", 27017}, {"localhost.test.build.10gen.cc.", 27018}},
         {}},

        {"mongodb+srv://user:password@test2.test.build.10gen.cc/"
         "database?someOption=someValue&someOtherOption=someOtherValue",
         "user",
         "password",
         "database",
         {{"localhost.test.build.10gen.cc.", 27018}, {"localhost.test.build.10gen.cc.", 27019}},
         {{"someOption", "someValue"}, {"someOtherOption", "someOtherValue"}}},


        {"mongodb+srv://user:password@test3.test.build.10gen.cc/"
         "database?someOption=someValue&someOtherOption=someOtherValue",
         "user",
         "password",
         "database",
         {{"localhost.test.build.10gen.cc.", 27017}},
         {{"someOption", "someValue"}, {"someOtherOption", "someOtherValue"}}},


        {"mongodb+srv://user:password@test5.test.build.10gen.cc/"
         "database?someOption=someValue&someOtherOption=someOtherValue",
         "user",
         "password",
         "database",
         {{"localhost.test.build.10gen.cc.", 27017}},
         {{"someOption", "someValue"},
          {"someOtherOption", "someOtherValue"},
          {"connectTimeoutMS", "300000"},
          {"socketTimeoutMS", "300000"}}},

        {"mongodb+srv://user:password@test5.test.build.10gen.cc/"
         "database?someOption=someValue&socketTimeoutMS=100&someOtherOption=someOtherValue",
         "user",
         "password",
         "database",
         {{"localhost.test.build.10gen.cc.", 27017}},
         {{"someOption", "someValue"},
          {"someOtherOption", "someOtherValue"},
          {"connectTimeoutMS", "300000"},
          {"socketTimeoutMS", "100"}}},

        {"mongodb+srv://test6.test.build.10gen.cc/",
         "",
         "",
         "",
         {{"localhost.test.build.10gen.cc.", 27017}},
         {{"connectTimeoutMS", "200000"}, {"socketTimeoutMS", "200000"}}},

        {"mongodb+srv://test6.test.build.10gen.cc/database",
         "",
         "",
         "database",
         {{"localhost.test.build.10gen.cc.", 27017}},
         {{"connectTimeoutMS", "200000"}, {"socketTimeoutMS", "200000"}}},

        {"mongodb+srv://test6.test.build.10gen.cc/?connectTimeoutMS=300000",
         "",
         "",
         "",
         {{"localhost.test.build.10gen.cc.", 27017}},
         {{"connectTimeoutMS", "300000"}, {"socketTimeoutMS", "200000"}}},

        {"mongodb+srv://test6.test.build.10gen.cc/?irrelevantOption=irrelevantValue",
         "",
         "",
         "",
         {{"localhost.test.build.10gen.cc.", 27017}},
         {{"connectTimeoutMS", "200000"},
          {"socketTimeoutMS", "200000"},
          {"irrelevantOption", "irrelevantValue"}}},


        {"mongodb+srv://test6.test.build.10gen.cc/"
         "?irrelevantOption=irrelevantValue&connectTimeoutMS=300000",
         "",
         "",
         "",
         {{"localhost.test.build.10gen.cc.", 27017}},
         {{"connectTimeoutMS", "300000"},
          {"socketTimeoutMS", "200000"},
          {"irrelevantOption", "irrelevantValue"}}},
    };

    for (const auto& test : tests) {
        auto rs = MongoURI::parse(test.uri);
        ASSERT_OK(rs.getStatus());
        auto rv = rs.getValue();
        ASSERT_EQ(rv.getUser(), test.user);
        ASSERT_EQ(rv.getPassword(), test.password);
        ASSERT_EQ(rv.getDatabase(), test.database);
        std::vector<std::pair<std::string, std::string>> options(begin(rv.getOptions()),
                                                                 end(rv.getOptions()));
        std::sort(begin(options), end(options));
        std::vector<std::pair<std::string, std::string>> expectedOptions(begin(test.options),
                                                                         end(test.options));
        std::sort(begin(expectedOptions), end(expectedOptions));

        for (std::size_t i = 0; i < std::min(options.size(), expectedOptions.size()); ++i) {
            if (options[i] != expectedOptions[i]) {
                mongo::unittest::log() << "Option: \"" << options[i].first << "="
                                       << options[i].second << "\" doesn't equal: \""
                                       << expectedOptions[i].first << "="
                                       << expectedOptions[i].second << "\"" << std::endl;
                std::cerr << "Failing URI: \"" << test.uri << "\"" << std::endl;
                ASSERT(false);
            }
        }
        ASSERT_EQ(options.size(), expectedOptions.size());

        std::vector<HostAndPort> hosts(begin(rv.getServers()), end(rv.getServers()));
        std::sort(begin(hosts), end(hosts));
        auto expectedHosts = test.hosts;
        std::sort(begin(expectedHosts), end(expectedHosts));

        for (std::size_t i = 0; i < std::min(hosts.size(), expectedHosts.size()); ++i) {
            ASSERT_EQ(hosts[i], expectedHosts[i]);
        }
        ASSERT_TRUE(hosts.size() == expectedHosts.size());
    }
}

}  // namespace
