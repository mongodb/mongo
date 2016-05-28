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

/**
 * This file contains tests for mongo/db/commands/mr.h
 */

#include "mongo/db/commands/mr.h"

#include <string>

#include "mongo/db/json.h"
#include "mongo/unittest/unittest.h"

using namespace mongo;

namespace {

/**
 * Tests for mr::Config
 */

/**
 * Helper function to verify field of mr::Config::OutputOptions.
 */
template <typename T>
void _compareOutputOptionField(const std::string& dbname,
                               const std::string& cmdObjStr,
                               const std::string& fieldName,
                               const T& actual,
                               const T& expected) {
    if (actual == expected)
        return;
    FAIL(str::stream() << "parseOutputOptions(\"" << dbname << ", " << cmdObjStr << "): "
                       << fieldName
                       << ": Expected: "
                       << expected
                       << ". Actual: "
                       << actual);
}

/**
 * Returns string representation of mr::Config::OutputType
 */
std::string _getOutTypeString(mr::Config::OutputType outType) {
    switch (outType) {
        case mr::Config::REPLACE:
            return "REPLACE";
        case mr::Config::MERGE:
            return "MERGE";
        case mr::Config::REDUCE:
            return "REDUCE";
        case mr::Config::INMEMORY:
            return "INMEMORY";
    }
    invariant(0);
}

/**
 * Test helper function to check expected result of parseOutputOptions.
 */
void _testConfigParseOutputOptions(const std::string& dbname,
                                   const std::string& cmdObjStr,
                                   const std::string& expectedOutDb,
                                   const std::string& expectedCollectionName,
                                   const std::string& expectedFinalNamespace,
                                   bool expectedOutNonAtomic,
                                   mr::Config::OutputType expectedOutType) {
    const BSONObj cmdObj = fromjson(cmdObjStr);
    mr::Config::OutputOptions outputOptions = mr::Config::parseOutputOptions(dbname, cmdObj);
    _compareOutputOptionField(dbname, cmdObjStr, "outDb", outputOptions.outDB, expectedOutDb);
    _compareOutputOptionField(
        dbname, cmdObjStr, "collectionName", outputOptions.collectionName, expectedCollectionName);
    _compareOutputOptionField(
        dbname, cmdObjStr, "finalNamespace", outputOptions.finalNamespace, expectedFinalNamespace);
    _compareOutputOptionField(
        dbname, cmdObjStr, "outNonAtomic", outputOptions.outNonAtomic, expectedOutNonAtomic);
    _compareOutputOptionField(dbname,
                              cmdObjStr,
                              "outType",
                              _getOutTypeString(outputOptions.outType),
                              _getOutTypeString(expectedOutType));
}

/**
 * Tests for mr::Config::parseOutputOptions.
 */
TEST(ConfigOutputOptionsTest, parseOutputOptions) {
    // Missing 'out' field.
    ASSERT_THROWS(mr::Config::parseOutputOptions("mydb", fromjson("{}")), UserException);
    // 'out' must be either string or object.
    ASSERT_THROWS(mr::Config::parseOutputOptions("mydb", fromjson("{out: 99}")), UserException);
    // 'out.nonAtomic' is not supported with normal, replace or inline.
    ASSERT_THROWS(mr::Config::parseOutputOptions(
                      "mydb", fromjson("{out: {normal: 'mycoll', nonAtomic: true}}")),
                  UserException);
    ASSERT_THROWS(mr::Config::parseOutputOptions(
                      "mydb", fromjson("{out: {replace: 'mycoll', nonAtomic: true}}")),
                  UserException);
    ASSERT_THROWS(mr::Config::parseOutputOptions(
                      "mydb", fromjson("{out: {inline: 'mycoll', nonAtomic: true}}")),
                  UserException);
    // Unknown output specifer.
    ASSERT_THROWS(
        mr::Config::parseOutputOptions("mydb", fromjson("{out: {no_such_out_type: 'mycoll'}}")),
        UserException);


    // 'out' is string.
    _testConfigParseOutputOptions(
        "mydb", "{out: 'mycoll'}", "", "mycoll", "mydb.mycoll", false, mr::Config::REPLACE);
    // 'out' is object.
    _testConfigParseOutputOptions("mydb",
                                  "{out: {normal: 'mycoll'}}",
                                  "",
                                  "mycoll",
                                  "mydb.mycoll",
                                  false,
                                  mr::Config::REPLACE);
    // 'out.db' overrides dbname parameter
    _testConfigParseOutputOptions("mydb1",
                                  "{out: {replace: 'mycoll', db: 'mydb2'}}",
                                  "mydb2",
                                  "mycoll",
                                  "mydb2.mycoll",
                                  false,
                                  mr::Config::REPLACE);
    // 'out.nonAtomic' is supported with merge and reduce.
    _testConfigParseOutputOptions("mydb",
                                  "{out: {merge: 'mycoll', nonAtomic: true}}",
                                  "",
                                  "mycoll",
                                  "mydb.mycoll",
                                  true,
                                  mr::Config::MERGE);
    _testConfigParseOutputOptions("mydb",
                                  "{out: {reduce: 'mycoll', nonAtomic: true}}",
                                  "",
                                  "mycoll",
                                  "mydb.mycoll",
                                  true,
                                  mr::Config::REDUCE);
    // inline
    _testConfigParseOutputOptions("mydb1",
                                  "{out: {inline: 'mycoll', db: 'mydb2'}}",
                                  "mydb2",
                                  "",
                                  "",
                                  false,
                                  mr::Config::INMEMORY);

    // Order should not matter in fields of 'out' object.
    _testConfigParseOutputOptions("mydb1",
                                  "{out: {db: 'mydb2', normal: 'mycoll'}}",
                                  "mydb2",
                                  "mycoll",
                                  "mydb2.mycoll",
                                  false,
                                  mr::Config::REPLACE);
    _testConfigParseOutputOptions("mydb1",
                                  "{out: {db: 'mydb2', replace: 'mycoll'}}",
                                  "mydb2",
                                  "mycoll",
                                  "mydb2.mycoll",
                                  false,
                                  mr::Config::REPLACE);
    _testConfigParseOutputOptions("mydb1",
                                  "{out: {nonAtomic: true, merge: 'mycoll'}}",
                                  "",
                                  "mycoll",
                                  "mydb1.mycoll",
                                  true,
                                  mr::Config::MERGE);
    _testConfigParseOutputOptions("mydb1",
                                  "{out: {nonAtomic: true, reduce: 'mycoll'}}",
                                  "",
                                  "mycoll",
                                  "mydb1.mycoll",
                                  true,
                                  mr::Config::REDUCE);
    _testConfigParseOutputOptions("mydb1",
                                  "{out: {db: 'mydb2', inline: 'mycoll'}}",
                                  "mydb2",
                                  "",
                                  "",
                                  false,
                                  mr::Config::INMEMORY);
}

TEST(ConfigTest, ParseCollation) {
    std::string dbname = "myDB";
    BSONObj collation = BSON("locale"
                             << "en_US");
    BSONObjBuilder bob;
    bob.append("mapReduce", "myCollection");
    bob.appendCode("map", "function() { emit(0, 1); }");
    bob.appendCode("reduce", "function(k, v) { return {count: 0}; }");
    bob.append("out", "outCollection");
    bob.append("collation", collation);
    BSONObj cmdObj = bob.obj();
    mr::Config config(dbname, cmdObj);
    ASSERT_EQUALS(config.collation, collation);
}

TEST(ConfigTest, ParseNoCollation) {
    std::string dbname = "myDB";
    BSONObjBuilder bob;
    bob.append("mapReduce", "myCollection");
    bob.appendCode("map", "function() { emit(0, 1); }");
    bob.appendCode("reduce", "function(k, v) { return {count: 0}; }");
    bob.append("out", "outCollection");
    BSONObj cmdObj = bob.obj();
    mr::Config config(dbname, cmdObj);
    ASSERT_EQUALS(config.collation, BSONObj());
}

TEST(ConfigTest, CollationNotAnObjectFailsToParse) {
    std::string dbname = "myDB";
    BSONObjBuilder bob;
    bob.append("mapReduce", "myCollection");
    bob.appendCode("map", "function() { emit(0, 1); }");
    bob.appendCode("reduce", "function(k, v) { return {count: 0}; }");
    bob.append("out", "outCollection");
    bob.append("collation", "en_US");
    BSONObj cmdObj = bob.obj();
    ASSERT_THROWS(mr::Config(dbname, cmdObj), UserException);
}

}  // namespace
