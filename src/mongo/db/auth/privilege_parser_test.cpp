/**
 *    Copyright (C) 2013 10gen Inc.
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
 * Unit tests of the ParsedPrivilege class.
 */

#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/privilege_parser.h"
#include "mongo/db/server_options.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(PrivilegeParserTest, IsValidTest) {
    ParsedPrivilege parsedPrivilege;
    std::string errmsg;

    // must have resource
    parsedPrivilege.parseBSON(BSON("actions" << BSON_ARRAY("find")), &errmsg);
    ASSERT_FALSE(parsedPrivilege.isValid(&errmsg));

    // must have actions
    parsedPrivilege.parseBSON(BSON("resource" << BSON("cluster" << true)), &errmsg);
    ASSERT_FALSE(parsedPrivilege.isValid(&errmsg));

    // resource can't have cluster as well as db or collection
    parsedPrivilege.parseBSON(BSON("resource" << BSON("cluster" << true << "db"
                                                                << ""
                                                                << "collection"
                                                                << "")
                                              << "actions"
                                              << BSON_ARRAY("find")),
                              &errmsg);
    ASSERT_FALSE(parsedPrivilege.isValid(&errmsg));

    // resource can't have db without collection
    parsedPrivilege.parseBSON(BSON("resource" << BSON("db"
                                                      << "")
                                              << "actions"
                                              << BSON_ARRAY("find")),
                              &errmsg);
    ASSERT_FALSE(parsedPrivilege.isValid(&errmsg));

    // resource can't have collection without db
    parsedPrivilege.parseBSON(BSON("resource" << BSON("collection"
                                                      << "")
                                              << "actions"
                                              << BSON_ARRAY("find")),
                              &errmsg);
    ASSERT_FALSE(parsedPrivilege.isValid(&errmsg));

    // Works with wildcard db and resource
    parsedPrivilege.parseBSON(BSON("resource" << BSON("db"
                                                      << ""
                                                      << "collection"
                                                      << "")
                                              << "actions"
                                              << BSON_ARRAY("find")),
                              &errmsg);
    ASSERT(parsedPrivilege.isValid(&errmsg));

    // Works with real db and collection
    parsedPrivilege.parseBSON(BSON("resource" << BSON("db"
                                                      << "test"
                                                      << "collection"
                                                      << "foo")
                                              << "actions"
                                              << BSON_ARRAY("find")),
                              &errmsg);
    ASSERT(parsedPrivilege.isValid(&errmsg));

    // Works with cluster resource
    parsedPrivilege.parseBSON(
        BSON("resource" << BSON("cluster" << true) << "actions" << BSON_ARRAY("find")), &errmsg);
    ASSERT(parsedPrivilege.isValid(&errmsg));
}

TEST(PrivilegeParserTest, ConvertBetweenPrivilegeTest) {
    ParsedPrivilege parsedPrivilege;
    Privilege privilege;
    std::string errmsg;
    std::vector<std::string> actionsVector;
    std::vector<std::string> unrecognizedActions;
    actionsVector.push_back("find");

    // Works with wildcard db and resource
    parsedPrivilege.parseBSON(BSON("resource" << BSON("db"
                                                      << ""
                                                      << "collection"
                                                      << "")
                                              << "actions"
                                              << BSON_ARRAY("find")),
                              &errmsg);
    ASSERT(parsedPrivilege.isValid(&errmsg));
    ASSERT_OK(ParsedPrivilege::parsedPrivilegeToPrivilege(
        parsedPrivilege, &privilege, &unrecognizedActions));
    ASSERT(unrecognizedActions.empty());
    ASSERT(privilege.getActions().contains(ActionType::find));
    ASSERT(!privilege.getActions().contains(ActionType::insert));
    ASSERT_EQUALS(privilege.getResourcePattern(), ResourcePattern::forAnyNormalResource());

    ASSERT(ParsedPrivilege::privilegeToParsedPrivilege(privilege, &parsedPrivilege, &errmsg));
    ASSERT(parsedPrivilege.isValid(&errmsg));
    ASSERT(parsedPrivilege.isResourceSet());
    ASSERT_FALSE(parsedPrivilege.getResource().isClusterSet());
    ASSERT(parsedPrivilege.getResource().isDbSet());
    ASSERT(parsedPrivilege.getResource().isCollectionSet());
    ASSERT_EQUALS("", parsedPrivilege.getResource().getDb());
    ASSERT_EQUALS("", parsedPrivilege.getResource().getCollection());
    ASSERT(parsedPrivilege.isActionsSet());
    ASSERT(actionsVector == parsedPrivilege.getActions());

    // Works with exact namespaces
    parsedPrivilege.parseBSON(BSON("resource" << BSON("db"
                                                      << "test"
                                                      << "collection"
                                                      << "foo")
                                              << "actions"
                                              << BSON_ARRAY("find")),
                              &errmsg);
    ASSERT(parsedPrivilege.isValid(&errmsg));
    ASSERT_OK(ParsedPrivilege::parsedPrivilegeToPrivilege(
        parsedPrivilege, &privilege, &unrecognizedActions));
    ASSERT(unrecognizedActions.empty());
    ASSERT(privilege.getActions().contains(ActionType::find));
    ASSERT(!privilege.getActions().contains(ActionType::insert));
    ASSERT_EQUALS(privilege.getResourcePattern(),
                  ResourcePattern::forExactNamespace(NamespaceString("test.foo")));

    ASSERT(ParsedPrivilege::privilegeToParsedPrivilege(privilege, &parsedPrivilege, &errmsg));
    ASSERT(parsedPrivilege.isValid(&errmsg));
    ASSERT(parsedPrivilege.isResourceSet());
    ASSERT_FALSE(parsedPrivilege.getResource().isClusterSet());
    ASSERT(parsedPrivilege.getResource().isDbSet());
    ASSERT(parsedPrivilege.getResource().isCollectionSet());
    ASSERT_EQUALS("test", parsedPrivilege.getResource().getDb());
    ASSERT_EQUALS("foo", parsedPrivilege.getResource().getCollection());
    ASSERT(parsedPrivilege.isActionsSet());
    ASSERT(actionsVector == parsedPrivilege.getActions());

    // Works with database resource
    parsedPrivilege.parseBSON(BSON("resource" << BSON("db"
                                                      << "test"
                                                      << "collection"
                                                      << "")
                                              << "actions"
                                              << BSON_ARRAY("find")),
                              &errmsg);
    ASSERT(parsedPrivilege.isValid(&errmsg));
    ASSERT_OK(ParsedPrivilege::parsedPrivilegeToPrivilege(
        parsedPrivilege, &privilege, &unrecognizedActions));
    ASSERT(unrecognizedActions.empty());
    ASSERT(privilege.getActions().contains(ActionType::find));
    ASSERT(!privilege.getActions().contains(ActionType::insert));
    ASSERT_EQUALS(privilege.getResourcePattern(), ResourcePattern::forDatabaseName("test"));

    ASSERT(ParsedPrivilege::privilegeToParsedPrivilege(privilege, &parsedPrivilege, &errmsg));
    ASSERT(parsedPrivilege.isValid(&errmsg));
    ASSERT(parsedPrivilege.isResourceSet());
    ASSERT_FALSE(parsedPrivilege.getResource().isClusterSet());
    ASSERT(parsedPrivilege.getResource().isDbSet());
    ASSERT(parsedPrivilege.getResource().isCollectionSet());
    ASSERT_EQUALS("test", parsedPrivilege.getResource().getDb());
    ASSERT_EQUALS("", parsedPrivilege.getResource().getCollection());
    ASSERT(parsedPrivilege.isActionsSet());
    ASSERT(actionsVector == parsedPrivilege.getActions());

    // Works with collection resource
    parsedPrivilege.parseBSON(BSON("resource" << BSON("db"
                                                      << ""
                                                      << "collection"
                                                      << "foo")
                                              << "actions"
                                              << BSON_ARRAY("find")),
                              &errmsg);
    ASSERT(parsedPrivilege.isValid(&errmsg));
    ASSERT_OK(ParsedPrivilege::parsedPrivilegeToPrivilege(
        parsedPrivilege, &privilege, &unrecognizedActions));
    ASSERT(unrecognizedActions.empty());
    ASSERT(privilege.getActions().contains(ActionType::find));
    ASSERT(!privilege.getActions().contains(ActionType::insert));
    ASSERT_EQUALS(privilege.getResourcePattern(), ResourcePattern::forCollectionName("foo"));

    ASSERT(ParsedPrivilege::privilegeToParsedPrivilege(privilege, &parsedPrivilege, &errmsg));
    ASSERT(parsedPrivilege.isValid(&errmsg));
    ASSERT(parsedPrivilege.isResourceSet());
    ASSERT_FALSE(parsedPrivilege.getResource().isClusterSet());
    ASSERT(parsedPrivilege.getResource().isDbSet());
    ASSERT(parsedPrivilege.getResource().isCollectionSet());
    ASSERT_EQUALS("", parsedPrivilege.getResource().getDb());
    ASSERT_EQUALS("foo", parsedPrivilege.getResource().getCollection());
    ASSERT(parsedPrivilege.isActionsSet());
    ASSERT(actionsVector == parsedPrivilege.getActions());

    // Works with cluster resource
    parsedPrivilege.parseBSON(
        BSON("resource" << BSON("cluster" << true) << "actions" << BSON_ARRAY("find")), &errmsg);
    ASSERT(parsedPrivilege.isValid(&errmsg));
    ASSERT_OK(ParsedPrivilege::parsedPrivilegeToPrivilege(
        parsedPrivilege, &privilege, &unrecognizedActions));
    ASSERT(unrecognizedActions.empty());
    ASSERT(privilege.getActions().contains(ActionType::find));
    ASSERT(!privilege.getActions().contains(ActionType::insert));
    ASSERT_EQUALS(privilege.getResourcePattern(), ResourcePattern::forClusterResource());

    ASSERT(ParsedPrivilege::privilegeToParsedPrivilege(privilege, &parsedPrivilege, &errmsg));
    ASSERT(parsedPrivilege.isValid(&errmsg));
    ASSERT(parsedPrivilege.isResourceSet());
    ASSERT(parsedPrivilege.getResource().isClusterSet());
    ASSERT(parsedPrivilege.getResource().getCluster());
    ASSERT_FALSE(parsedPrivilege.getResource().isDbSet());
    ASSERT_FALSE(parsedPrivilege.getResource().isCollectionSet());
    ASSERT(parsedPrivilege.isActionsSet());
    ASSERT(actionsVector == parsedPrivilege.getActions());
}

TEST(PrivilegeParserTest, ParseInvalidActionsTest) {
    ParsedPrivilege parsedPrivilege;
    Privilege privilege;
    std::string errmsg;
    std::vector<std::string> actionsVector;
    std::vector<std::string> unrecognizedActions;
    actionsVector.push_back("find");

    parsedPrivilege.parseBSON(BSON("resource" << BSON("db"
                                                      << ""
                                                      << "collection"
                                                      << "")
                                              << "actions"
                                              << BSON_ARRAY("find"
                                                            << "fakeAction")),
                              &errmsg);
    ASSERT(parsedPrivilege.isValid(&errmsg));
    ASSERT_OK(ParsedPrivilege::parsedPrivilegeToPrivilege(
        parsedPrivilege, &privilege, &unrecognizedActions));
    ASSERT(privilege.getActions().contains(ActionType::find));
    ASSERT(!privilege.getActions().contains(ActionType::insert));
    ASSERT_EQUALS(privilege.getResourcePattern(), ResourcePattern::forAnyNormalResource());
    ASSERT_EQUALS(1U, unrecognizedActions.size());
    ASSERT_EQUALS("fakeAction", unrecognizedActions[0]);
}
}  // namespace
}  // namespace mongo
