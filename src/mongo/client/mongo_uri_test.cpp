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

#include "mongo/client/mongo_uri.h"

#include "mongo/unittest/unittest.h"

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

    {"mongodb://127.0.0.1/dbName?foo=a&c=b", "", "", kMaster, "", 1, 2, "dbName"},

    {"mongodb://localhost/?foo=bar", "", "", kMaster, "", 1, 1, ""},

    {"mongodb://user:pwd@127.0.0.1:1234", "user", "pwd", kMaster, "", 1, 0, ""},

    {"mongodb://user@127.0.0.1:1234", "user", "", kMaster, "", 1, 0, ""},

    {"mongodb://127.0.0.1:1234/dbName?foo=a&c=b", "", "", kMaster, "", 1, 2, "dbName"},

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
    {"mongodb:///tmp/mongodb-27017.sock", "", "", kMaster, "", 1, 0, ""},

    {"mongodb:///tmp/mongodb-27017.sock,/tmp/mongodb-27018.sock/?replicaSet=replName",
     "",
     "",
     kSet,
     "replName",
     2,
     1,
     ""}};

const InvalidURITestCase invalidCases[] = {

    // No host.
    {"mongodb://"},

    // Needs a "/" after the hosts and before the options.
    {"mongodb://localhost:27017,localhost:27018?replicaSet=missingSlash"},

    // Host list must actually be comma separated.
    {"mongodb://localhost:27017localhost:27018"},

    // Domain sockets have to end in ".sock".
    {"mongodb:///notareal/domainsock"},

    // Options can't have multiple question marks. Only one.
    {"mongodb://localhost:27017/?foo=a?c=b&d=e?asdf=foo"},
};

TEST(MongoURI, GoodTrickyURIs) {
    const size_t numCases = sizeof(validCases) / sizeof(validCases[0]);

    for (size_t i = 0; i != numCases; ++i) {
        const URITestCase testCase = validCases[i];
        mongo::unittest::log() << "Testing URI: " << testCase.URI << '\n';
        std::string errMsg;
        auto cs_status = MongoURI::parse(testCase.URI);
        if (!cs_status.getStatus().toString().empty()) {
            mongo::unittest::log() << "error with uri: " << cs_status.getStatus().toString();
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
    auto dbclient = uri.connect(errmsg);
    ASSERT_EQ(dbclient, static_cast<decltype(dbclient)>(nullptr));
}

}  // namespace
