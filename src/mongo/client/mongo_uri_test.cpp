/**
 *    Copyright (C) 2009-2015 MongoDB Inc.
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

    {"mongodb://user:pwd@127.0.0.1:1234", "user", "pwd", kMaster, "", 1, 0, ""},

    {"mongodb://user@127.0.0.1:1234", "user", "", kMaster, "", 1, 0, ""},

    {"mongodb://127.0.0.1:1234/dbName?foo=a&c=b", "", "", kMaster, "", 1, 2, "dbName"},

    {"mongodb://127.0.0.1/dbName?foo=a&c=b", "", "", kMaster, "", 1, 2, "dbName"},

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
    {"mongodb:///notareal/domainsock"},

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
    ASSERT(obj.valid(mongo::BSONVersion::kLatest));
    ASSERT(obj.hasField("tests"));
    mongo::BSONObj arr = obj.getField("tests").embeddedObject().getOwned();
    ASSERT(arr.couldBeArray());
    return arr;
}

// Helper method to take a BSONElement and either extract its string or return an empty string
std::string returnStringFromElementOrNull(mongo::BSONElement element) {
    ASSERT(!element.eoo());
    if (element.type() == mongo::jstNULL) {
        return std::string();
    }
    ASSERT(element.type() == mongo::String);
    return element.String();
}

// Helper method to take a valid test case, parse() it, and assure the output is correct
void testValidURIFormat(URITestCase testCase) {
    mongo::unittest::log() << "Testing URI: " << testCase.URI << '\n';
    std::string errMsg;
    auto cs_status = MongoURI::parse(testCase.URI);
    if (!cs_status.getStatus().toString().empty()) {
        if (!cs_status.getStatus().isOK())
            mongo::unittest::log() << "ERROR: error with uri: " << cs_status.getStatus().toString();
    }
    ASSERT_TRUE(cs_status.isOK());
    auto result = cs_status.getValue();
    ASSERT_EQ(testCase.uname, result.getUser());
    ASSERT_EQ(testCase.password, result.getPassword());
    ASSERT_EQ(testCase.type, result.type());
    ASSERT_EQ(testCase.setname, result.getSetName());
    ASSERT_EQ(testCase.numservers, result.getServers().size());
    ASSERT_EQ(testCase.numOptions, result.getOptions().size());
    ASSERT_EQ(testCase.database, result.getDatabase());
}

// Helper method to parse a BSON array/object and extract the individual tests
// Method creates a URITestCase from every element in the array and then verifies that parse() has
// the proper output
void runTests(mongo::BSONObj tests) {
    mongo::BSONObjIterator testsIter(tests);
    while (testsIter.more()) {
        mongo::BSONElement testElement = testsIter.next();
        if (testElement.eoo())
            break;
        mongo::BSONObj test = testElement.embeddedObject();

        // First extract the valid field and the uri field
        mongo::BSONElement validDoc = test.getField("valid");
        ASSERT(!validDoc.eoo());
        ASSERT(validDoc.isBoolean());
        bool valid = validDoc.Bool();

        mongo::BSONElement uriDoc = test.getField("uri");
        ASSERT(!uriDoc.eoo());
        ASSERT(uriDoc.type() == mongo::String);
        std::string uri = uriDoc.String();

        if (!valid) {
            // This uri string is invalid --> parse the uri and ensure it fails
            const InvalidURITestCase testCase = {uri};
            mongo::unittest::log() << "Testing URI: " << testCase.URI << '\n';
            auto cs_status = MongoURI::parse(testCase.URI);
            ASSERT_FALSE(cs_status.isOK());
        } else {
            // This uri is valid -- > parse the remaining necessary fields

            // parse the auth options
            std::string database = std::string();
            std::string username = std::string();
            std::string password = std::string();

            mongo::BSONElement auth = test.getField("auth");
            ASSERT(!auth.eoo());
            if (auth.type() != mongo::jstNULL) {
                ASSERT(auth.type() == mongo::Object);
                mongo::BSONObj authObj = auth.embeddedObject();

                mongo::BSONElement dbObj = authObj.getField("db");
                database = returnStringFromElementOrNull(dbObj);

                mongo::BSONElement usernameObj = authObj.getField("username");
                username = returnStringFromElementOrNull(usernameObj);

                mongo::BSONElement passwordObj = authObj.getField("password");
                password = returnStringFromElementOrNull(passwordObj);
            }

            // parse the hosts
            size_t numHosts = 0;
            mongo::BSONElement hosts = test.getField("hosts");
            ASSERT(!hosts.eoo());
            ASSERT(hosts.type() == mongo::Array);
            mongo::BSONObjIterator hostsIter(hosts.embeddedObject());
            while (hostsIter.more()) {
                mongo::BSONElement cHost = hostsIter.next();
                if (cHost.eoo())
                    break;
                numHosts++;
            }

            // parse the options
            mongo::ConnectionString::ConnectionType connectionType = kMaster;
            size_t numOptions = 0;
            std::string setName = std::string();
            mongo::BSONElement optionsElement = test.getField("options");
            ASSERT(!optionsElement.eoo());
            if (optionsElement.type() != mongo::jstNULL) {
                ASSERT(optionsElement.type() == mongo::Object);
                mongo::BSONObj optionsObj = optionsElement.embeddedObject();
                numOptions = optionsObj.nFields();
                mongo::BSONElement replsetElement = optionsObj.getField("replicaSet");
                if (!replsetElement.eoo()) {
                    ASSERT(replsetElement.type() == mongo::String);
                    setName = replsetElement.String();
                    connectionType = kSet;
                }
            }

            // Create the URITestCase abnd
            const URITestCase testCase = {
                uri, username, password, connectionType, setName, numHosts, numOptions, database};
            testValidURIFormat(testCase);
        }
    }
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
        ASSERT_FALSE(cs_status.isOK());
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

/*  These tests come from the Mongo Uri Specifications for the drivers found at:
    https://github.com/mongodb/specifications/tree/master/source/connection-string/tests
    They have been slighly altered as the Drivers specification is slighly different from the server
   specification
*/
TEST(MongoURI, ValidAuth) {
    std::string fileName = "mongo-uri-valid-auth.json";
    mongo::BSONObj tests = getBsonFromJsonFile(fileName);
    runTests(tests);
}

TEST(MongoURI, Options) {
    std::string fileName = "mongo-uri-options.json";
    mongo::BSONObj tests = getBsonFromJsonFile(fileName);
    runTests(tests);
}

TEST(MongoURI, UnixSocketsAbsolute) {
    std::string fileName = "mongo-uri-unix-sockets-absolute.json";
    mongo::BSONObj tests = getBsonFromJsonFile(fileName);
    runTests(tests);
}

TEST(MongoURI, UnixSocketsRelative) {
    std::string fileName = "mongo-uri-unix-sockets-relative.json";
    mongo::BSONObj tests = getBsonFromJsonFile(fileName);
    runTests(tests);
}

TEST(MongoURI, Warnings) {
    std::string fileName = "mongo-uri-warnings.json";
    mongo::BSONObj tests = getBsonFromJsonFile(fileName);
    runTests(tests);
}

TEST(MongoURI, HostIdentifiers) {
    std::string fileName = "mongo-uri-host-identifiers.json";
    mongo::BSONObj tests = getBsonFromJsonFile(fileName);
    runTests(tests);
}

TEST(MongoURI, Invalid) {
    std::string fileName = "mongo-uri-invalid.json";
    mongo::BSONObj tests = getBsonFromJsonFile(fileName);
    runTests(tests);
}

}  // namespace
