/*    Copyright 2012 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

/**
 * Unit tests of the AuthorizationSession type.
 */

#include "mongo/base/status.h"
#include "mongo/db/auth/authz_session_external_state_mock.h"
#include "mongo/db/auth/authz_manager_external_state_mock.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespacestring.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/map_util.h"

#define ASSERT_NULL(EXPR) ASSERT_FALSE(EXPR)
#define ASSERT_NON_NULL(EXPR) ASSERT_TRUE(EXPR)

namespace mongo {
namespace {

    TEST(AuthorizationSessionTest, AcquirePrivilegeAndCheckAuthorization) {
        Principal* principal = new Principal(UserName("Spencer", "test"));
        ActionSet actions;
        actions.addAction(ActionType::insert);
        Privilege writePrivilege("test", actions);
        Privilege allDBsWritePrivilege("*", actions);
        AuthzManagerExternalStateMock* managerExternalState = new AuthzManagerExternalStateMock();
        AuthorizationManager authManager(managerExternalState);
        AuthzSessionExternalStateMock* sessionExternalState = new AuthzSessionExternalStateMock(
                &authManager);
        AuthorizationSession authzSession(sessionExternalState);

        ASSERT_FALSE(authzSession.checkAuthorization("test", ActionType::insert));
        sessionExternalState->setReturnValueForShouldIgnoreAuthChecks(true);
        ASSERT_TRUE(authzSession.checkAuthorization("test", ActionType::insert));
        sessionExternalState->setReturnValueForShouldIgnoreAuthChecks(false);
        ASSERT_FALSE(authzSession.checkAuthorization("test", ActionType::insert));

        ASSERT_EQUALS(ErrorCodes::UserNotFound,
                      authzSession.acquirePrivilege(writePrivilege, principal->getName()));
        authzSession.addAuthorizedPrincipal(principal);
        ASSERT_OK(authzSession.acquirePrivilege(writePrivilege, principal->getName()));
        ASSERT_TRUE(authzSession.checkAuthorization("test", ActionType::insert));

        ASSERT_FALSE(authzSession.checkAuthorization("otherDb", ActionType::insert));
        ASSERT_OK(authzSession.acquirePrivilege(allDBsWritePrivilege, principal->getName()));
        ASSERT_TRUE(authzSession.checkAuthorization("otherDb", ActionType::insert));
        // Auth checks on a collection should be applied to the database name.
        ASSERT_TRUE(authzSession.checkAuthorization("otherDb.collectionName", ActionType::insert));

        authzSession.logoutDatabase("test");
        ASSERT_FALSE(authzSession.checkAuthorization("test", ActionType::insert));
    }

    TEST(AuthorizationSessionTest, GetPrivilegesFromPrivilegeDocumentCompatible) {
        UserName user("Spencer", "test");
        BSONObj invalid;
        BSONObj readWrite = BSON("user" << "Spencer" << "pwd" << "passwordHash");
        BSONObj readOnly = BSON("user" << "Spencer" << "pwd" << "passwordHash" <<
                                "readOnly" << true);

        PrivilegeSet privilegeSet;
        ASSERT_EQUALS(ErrorCodes::UnsupportedFormat,
                      AuthorizationSession::buildPrivilegeSet("test",
                                                              user,
                                                              invalid,
                                                              &privilegeSet).code());

        ASSERT_OK(AuthorizationSession::buildPrivilegeSet("test",
                                                          user,
                                                          readOnly,
                                                          &privilegeSet));
        ASSERT(!privilegeSet.hasPrivilege(Privilege("test", ActionType::insert)));
        ASSERT(privilegeSet.hasPrivilege(Privilege("test", ActionType::find)));

        ASSERT_OK(AuthorizationSession::buildPrivilegeSet("test",
                                                          user,
                                                          readWrite,
                                                          &privilegeSet));
        ASSERT(privilegeSet.hasPrivilege(Privilege("test", ActionType::find)));
        ASSERT(privilegeSet.hasPrivilege(Privilege("test", ActionType::insert)));
        ASSERT(privilegeSet.hasPrivilege(Privilege("test", ActionType::userAdmin)));
        ASSERT(privilegeSet.hasPrivilege(Privilege("test", ActionType::compact)));
        ASSERT(!privilegeSet.hasPrivilege(Privilege("test", ActionType::shutdown)));
        ASSERT(!privilegeSet.hasPrivilege(Privilege("test", ActionType::addShard)));
        ASSERT(!privilegeSet.hasPrivilege(Privilege("admin", ActionType::find)));
        ASSERT(!privilegeSet.hasPrivilege(Privilege("*", ActionType::find)));

        ASSERT_OK(AuthorizationSession::buildPrivilegeSet("admin",
                                                          user,
                                                          readOnly,
                                                          &privilegeSet));
        // Should grant privileges on *.
        ASSERT(privilegeSet.hasPrivilege(Privilege("*", ActionType::find)));

        ASSERT(!privilegeSet.hasPrivilege(Privilege("admin", ActionType::insert)));
        ASSERT(!privilegeSet.hasPrivilege(Privilege("*", ActionType::insert)));

        ASSERT_OK(AuthorizationSession::buildPrivilegeSet("admin",
                                                          user,
                                                          readWrite,
                                                          &privilegeSet));
        ASSERT(privilegeSet.hasPrivilege(Privilege("*", ActionType::insert)));
    }

    class PrivilegeDocumentParsing : public ::mongo::unittest::Test {
    public:
        PrivilegeDocumentParsing() : user("spencer", "test") {}

        UserName user;
        PrivilegeSet privilegeSet;
    };

    TEST_F(PrivilegeDocumentParsing, VerifyRolesFieldMustBeAnArray) {
        ASSERT_NOT_OK(AuthorizationSession::buildPrivilegeSet(
                              "test",
                              user,
                              BSON("user" << "spencer" << "pwd" << "" <<
                                   "roles" << "read"),
                              &privilegeSet));
        ASSERT(!privilegeSet.hasPrivilege(Privilege("test", ActionType::find)));
    }

    TEST_F(PrivilegeDocumentParsing, VerifyInvalidRoleGrantsNoPrivileges) {
        ASSERT_OK(AuthorizationSession::buildPrivilegeSet(
                              "test",
                              user,
                              BSON("user" << "spencer" << "pwd" << "" <<
                                   "roles" << BSON_ARRAY("frim")),
                              &privilegeSet));
        ASSERT(!privilegeSet.hasPrivilege(Privilege("test", ActionType::find)));
    }

    TEST_F(PrivilegeDocumentParsing, VerifyInvalidRoleStillAllowsOtherRoles) {
        ASSERT_OK(AuthorizationSession::buildPrivilegeSet(
                              "test",
                              user,
                              BSON("user" << "spencer" << "pwd" << "" <<
                                   "roles" << BSON_ARRAY("read" << "frim")),
                              &privilegeSet));
        ASSERT(privilegeSet.hasPrivilege(Privilege("test", ActionType::find)));
    }

    TEST_F(PrivilegeDocumentParsing, VerifyCannotGrantClusterAdminRoleFromNonAdminDatabase) {
        ASSERT_OK(AuthorizationSession::buildPrivilegeSet(
                              "test",
                              user,
                              BSON("user" << "spencer" << "pwd" << "" <<
                                   "roles" << BSON_ARRAY("read" << "clusterAdmin")),
                              &privilegeSet));
        ASSERT(privilegeSet.hasPrivilege(Privilege("test", ActionType::find)));
        ASSERT(!privilegeSet.hasPrivilege(Privilege("test", ActionType::shutdown)));
        ASSERT(!privilegeSet.hasPrivilege(Privilege("test", ActionType::dropDatabase)));
    }

    TEST_F(PrivilegeDocumentParsing, VerifyCannotGrantClusterReadFromNonAdminDatabase) {
        ASSERT_OK(AuthorizationSession::buildPrivilegeSet(
                              "test",
                              user,
                              BSON("user" << "spencer" << "pwd" << "" <<
                                   "roles" << BSON_ARRAY("read" << "readAnyDatabase")),
                              &privilegeSet));
        ASSERT(privilegeSet.hasPrivilege(Privilege("test", ActionType::find)));
        ASSERT(!privilegeSet.hasPrivilege(Privilege("test2", ActionType::find)));
    }

    TEST_F(PrivilegeDocumentParsing, VerifyCannotGrantClusterReadWriteFromNonAdminDatabase) {
        ASSERT_OK(AuthorizationSession::buildPrivilegeSet(
                              "test",
                              user,
                              BSON("user" << "spencer" << "pwd" << "" <<
                                   "roles" << BSON_ARRAY("read" << "readWriteAnyDatabase")),
                              &privilegeSet));
        ASSERT(privilegeSet.hasPrivilege(Privilege("test", ActionType::find)));
        ASSERT(!privilegeSet.hasPrivilege(Privilege("test", ActionType::insert)));
        ASSERT(!privilegeSet.hasPrivilege(Privilege("test2", ActionType::insert)));
    }

    TEST_F(PrivilegeDocumentParsing, VerifyCannotGrantClusterUserAdminFromNonAdminDatabase) {
        ASSERT_OK(AuthorizationSession::buildPrivilegeSet(
                              "test",
                              user,
                              BSON("user" << "spencer" << "pwd" << "" <<
                                   "roles" << BSON_ARRAY("read" << "userAdminAnyDatabase")),
                              &privilegeSet));
        ASSERT(privilegeSet.hasPrivilege(Privilege("test", ActionType::find)));
        ASSERT(!privilegeSet.hasPrivilege(Privilege("test", ActionType::userAdmin)));
        ASSERT(!privilegeSet.hasPrivilege(Privilege("test2", ActionType::userAdmin)));
    }

    TEST_F(PrivilegeDocumentParsing, VerifyCannotGrantClusterDBAdminFromNonAdminDatabase) {
        ASSERT_OK(AuthorizationSession::buildPrivilegeSet(
                              "test",
                              user,
                              BSON("user" << "spencer" << "pwd" << "" <<
                                   "roles" << BSON_ARRAY("read" << "dbAdminAnyDatabase")),
                              &privilegeSet));
        ASSERT(privilegeSet.hasPrivilege(Privilege("test", ActionType::find)));
        ASSERT(!privilegeSet.hasPrivilege(Privilege("test", ActionType::clean)));
        ASSERT(!privilegeSet.hasPrivilege(Privilege("test2", ActionType::clean)));
    }

    TEST_F(PrivilegeDocumentParsing, VerifyOtherDBRolesMustBeAnObjectOfArraysOfStrings) {
        ASSERT_NOT_OK(AuthorizationSession::buildPrivilegeSet(
                              "admin",
                              user,
                              BSON("user" << "spencer" << "pwd" << "" <<
                                   "roles" << BSON_ARRAY("read") <<
                                   "otherDBRoles" << BSON_ARRAY("read")),
                              &privilegeSet));
        ASSERT(!privilegeSet.hasPrivilege(Privilege("test", ActionType::find)));
        ASSERT(!privilegeSet.hasPrivilege(Privilege("test2", ActionType::find)));
        ASSERT(!privilegeSet.hasPrivilege(Privilege("admin", ActionType::find)));

        ASSERT_NOT_OK(AuthorizationSession::buildPrivilegeSet(
                              "admin",
                              user,
                              BSON("user" << "spencer" << "pwd" << "" <<
                                   "roles" << BSON_ARRAY("read") <<
                                   "otherDBRoles" << BSON("test2" << "read")),
                              &privilegeSet));
        ASSERT(!privilegeSet.hasPrivilege(Privilege("test", ActionType::find)));
        ASSERT(!privilegeSet.hasPrivilege(Privilege("test2", ActionType::find)));
        ASSERT(!privilegeSet.hasPrivilege(Privilege("admin", ActionType::find)));
    }

    TEST_F(PrivilegeDocumentParsing, VerifyCannotGrantPrivilegesOnOtherDatabasesNormally) {
        // Cannot grant privileges on other databases, except from admin database.
        ASSERT_NOT_OK(AuthorizationSession::buildPrivilegeSet(
                              "test",
                              user,
                              BSON("user" << "spencer" << "pwd" << "" <<
                                   "roles" << BSON_ARRAY("read") <<
                                   "otherDBRoles" << BSON("test2" << BSON_ARRAY("read"))),
                              &privilegeSet));
        ASSERT(!privilegeSet.hasPrivilege(Privilege("test", ActionType::find)));
        ASSERT(!privilegeSet.hasPrivilege(Privilege("test2", ActionType::find)));
    }

    TEST_F(PrivilegeDocumentParsing, SuccessfulSimpleReadGrant) {
        // Grant read on test.
        ASSERT_OK(AuthorizationSession::buildPrivilegeSet(
                          "test",
                          user,
                          BSON("user" << "spencer" << "pwd" << "" << "roles" << BSON_ARRAY("read")),
                          &privilegeSet));

        ASSERT(privilegeSet.hasPrivilege(Privilege("test", ActionType::find)));
        ASSERT(!privilegeSet.hasPrivilege(Privilege("test2", ActionType::find)));
        ASSERT(!privilegeSet.hasPrivilege(Privilege("admin", ActionType::find)));
    }

    TEST_F(PrivilegeDocumentParsing, SuccessfulSimpleUserAdminTest) {
        // Grant userAdmin on "test" database.
        ASSERT_OK(AuthorizationSession::buildPrivilegeSet(
                          "test",
                          user,
                          BSON("user" << "spencer" << "pwd" << "" <<
                               "roles" << BSON_ARRAY("userAdmin")),
                          &privilegeSet));
        ASSERT(privilegeSet.hasPrivilege(Privilege("test", ActionType::userAdmin)));
        ASSERT(!privilegeSet.hasPrivilege(Privilege("test2", ActionType::userAdmin)));
        ASSERT(!privilegeSet.hasPrivilege(Privilege("admin", ActionType::userAdmin)));
    }

    TEST_F(PrivilegeDocumentParsing, GrantUserAdminOnAdmin) {
        // Grant userAdmin on admin.
        ASSERT_OK(AuthorizationSession::buildPrivilegeSet(
                          "admin",
                          user,
                          BSON("user" << "spencer" << "pwd" << "" <<
                               "roles" << BSON_ARRAY("userAdmin")),
                          &privilegeSet));
        ASSERT(!privilegeSet.hasPrivilege(Privilege("test", ActionType::userAdmin)));
        ASSERT(!privilegeSet.hasPrivilege(Privilege("test2", ActionType::userAdmin)));
        ASSERT(privilegeSet.hasPrivilege(Privilege("admin", ActionType::userAdmin)));
    }

    TEST_F(PrivilegeDocumentParsing, GrantUserAdminOnTestViaAdmin) {
        // Grant userAdmin on test via admin.
        ASSERT_OK(AuthorizationSession::buildPrivilegeSet(
                          "admin",
                          user,
                          BSON("user" << "spencer" << "pwd" << "" <<
                               "roles" << BSONArrayBuilder().arr() <<
                               "otherDBRoles" << BSON("test" << BSON_ARRAY("userAdmin"))),
                          &privilegeSet));
        ASSERT(privilegeSet.hasPrivilege(Privilege("test", ActionType::userAdmin)));
        ASSERT(!privilegeSet.hasPrivilege(Privilege("test2", ActionType::userAdmin)));
        ASSERT(!privilegeSet.hasPrivilege(Privilege("admin", ActionType::userAdmin)));
    }

    TEST_F(PrivilegeDocumentParsing, SuccessfulClusterAdminTest) {
        // Grant userAdminAnyDatabase.
        ASSERT_OK(AuthorizationSession::buildPrivilegeSet(
                          "admin",
                          user,
                          BSON("user" << "spencer" << "pwd" << "" <<
                               "roles" << BSON_ARRAY("userAdminAnyDatabase")),
                          &privilegeSet));
        ASSERT(privilegeSet.hasPrivilege(Privilege("test", ActionType::userAdmin)));
        ASSERT(privilegeSet.hasPrivilege(Privilege("test2", ActionType::userAdmin)));
        ASSERT(privilegeSet.hasPrivilege(Privilege("admin", ActionType::userAdmin)));
    }


    TEST_F(PrivilegeDocumentParsing, GrantClusterReadWrite) {
        // Grant readWrite on everything via the admin database.
        ASSERT_OK(AuthorizationSession::buildPrivilegeSet(
                          "admin",
                          user,
                          BSON("user" << "spencer" << "pwd" << "" <<
                               "roles" << BSON_ARRAY("readWriteAnyDatabase")),
                          &privilegeSet));
        ASSERT(privilegeSet.hasPrivilege(Privilege("test", ActionType::find)));
        ASSERT(privilegeSet.hasPrivilege(Privilege("test2", ActionType::find)));
        ASSERT(privilegeSet.hasPrivilege(Privilege("admin", ActionType::find)));
        ASSERT(privilegeSet.hasPrivilege(Privilege("test", ActionType::insert)));
        ASSERT(privilegeSet.hasPrivilege(Privilege("test2", ActionType::insert)));
        ASSERT(privilegeSet.hasPrivilege(Privilege("admin", ActionType::insert)));
    }

    TEST_F(PrivilegeDocumentParsing, ProhibitGrantOnWildcard) {
        // Cannot grant readWrite to everythign using "otherDBRoles".
        ASSERT_NOT_OK(AuthorizationSession::buildPrivilegeSet(
                          "admin",
                          user,
                          BSON("user" << "spencer" << "pwd" << "" <<
                               "roles" << BSONArrayBuilder().arr() <<
                               "otherDBRoles" << BSON("*" << BSON_ARRAY("readWrite"))),
                          &privilegeSet));
        ASSERT(!privilegeSet.hasPrivilege(Privilege("test", ActionType::find)));
        ASSERT(!privilegeSet.hasPrivilege(Privilege("test2", ActionType::find)));
        ASSERT(!privilegeSet.hasPrivilege(Privilege("admin", ActionType::find)));
        ASSERT(!privilegeSet.hasPrivilege(Privilege("test", ActionType::insert)));
        ASSERT(!privilegeSet.hasPrivilege(Privilege("test2", ActionType::insert)));
        ASSERT(!privilegeSet.hasPrivilege(Privilege("admin", ActionType::insert)));
    }

    TEST_F(PrivilegeDocumentParsing, GrantClusterAdmin) {
        // Grant cluster admin
        ASSERT_OK(AuthorizationSession::buildPrivilegeSet(
                          "admin",
                          user,
                          BSON("user" << "spencer" << "pwd" << "" <<
                               "roles" << BSON_ARRAY("clusterAdmin")),
                          &privilegeSet));
        ASSERT(privilegeSet.hasPrivilege(Privilege("test", ActionType::dropDatabase)));
        ASSERT(privilegeSet.hasPrivilege(Privilege("test2", ActionType::dropDatabase)));
        ASSERT(privilegeSet.hasPrivilege(Privilege("admin", ActionType::dropDatabase)));
        ASSERT(privilegeSet.hasPrivilege(Privilege("$SERVER", ActionType::shutdown)));
        ASSERT(privilegeSet.hasPrivilege(Privilege("$CLUSTER", ActionType::moveChunk)));
    }

    TEST(AuthorizationSessionTest, GetPrivilegesFromPrivilegeDocumentInvalid) {
        BSONObj oldAndNewMixed = BSON("user" << "spencer" <<
                                      "pwd" << "passwordHash" <<
                                      "readOnly" << false <<
                                      "roles" << BSON_ARRAY("write" << "userAdmin"));

        UserName user("spencer", "anydb");
        PrivilegeSet result;
        ASSERT_NOT_OK(AuthorizationSession::buildPrivilegeSet(
                              "anydb", user, oldAndNewMixed, &result));
    }

    TEST(AuthorizationSessionTest, DocumentValidationCompatibility) {
        Status (*check)(const StringData&, const BSONObj&) =
            &AuthorizationSession::checkValidPrivilegeDocument;

        // Good documents, with and without "readOnly" fields.
        ASSERT_OK(check("test", BSON("user" << "andy" << "pwd" << "a")));
        ASSERT_OK(check("test", BSON("user" << "andy" << "pwd" << "a" << "readOnly" << 1)));
        ASSERT_OK(check("test", BSON("user" << "andy" << "pwd" << "a" << "readOnly" << false)));
        ASSERT_OK(check("test", BSON("user" << "andy" << "pwd" << "a" << "readOnly" << "yes")));

        // Must have a "pwd" field.
        ASSERT_NOT_OK(check("test", BSON("user" << "andy")));

        // "pwd" field must be a string.
        ASSERT_NOT_OK(check("test", BSON("user" << "andy" << "pwd" << 100)));

        // "pwd" field string must not be empty.
        ASSERT_NOT_OK(check("test", BSON("user" << "andy" << "pwd" << "")));

        // Must have a "user" field.
        ASSERT_NOT_OK(check("test", BSON("pwd" << "a")));

        // "user" field must be a string.
        ASSERT_NOT_OK(check("test", BSON("user" << 100 << "pwd" << "a")));

        // "user" field string must not be empty.
        ASSERT_NOT_OK(check("test", BSON("user" << "" << "pwd" << "a")));
    }


    class CompatibilityModeDisabler {
    public:
        CompatibilityModeDisabler() {
            AuthorizationManager::setSupportOldStylePrivilegeDocuments(false);
        }
        ~CompatibilityModeDisabler() {
            AuthorizationManager::setSupportOldStylePrivilegeDocuments(true);
        }
    };

    TEST(AuthorizationSessionTest, DisableCompatibilityMode) {
        Status (*check)(const StringData&, const BSONObj&) =
            &AuthorizationSession::checkValidPrivilegeDocument;

        CompatibilityModeDisabler disabler;

        ASSERT_NOT_OK(check("test", BSON("user" << "andy" << "pwd" << "a")));
        ASSERT_NOT_OK(check("test", BSON("user" << "andy" << "pwd" << "a" << "readOnly" << 1)));
        ASSERT_NOT_OK(check("test", BSON("user" << "andy" << "pwd" << "a" << "readOnly" << false)));
        ASSERT_NOT_OK(check("test", BSON("user" << "andy" << "pwd" << "a" << "readOnly" << "yes")));

        ASSERT_OK(check("test", BSON("user" << "andy" << "pwd" << "a" <<
                                     "roles" << BSON_ARRAY("dbAdmin" << "read"))));
    }

    TEST(AuthorizationSessionTest, DocumentValidationExtended) {
        Status (*check)(const StringData&, const BSONObj&) =
            &AuthorizationSession::checkValidPrivilegeDocument;

        // Document describing new-style user on "test".
        ASSERT_OK(check("test", BSON("user" << "andy" << "pwd" << "a" <<
                                     "roles" << BSON_ARRAY("read"))));

        // Document giving roles on "test" to a user from "test2".
        ASSERT_OK(check("test", BSON("user" << "andy" << "userSource" << "test2" <<
                                     "roles" << BSON_ARRAY("read"))));

        // Cannot have "userSource" field value == dbname.
        ASSERT_NOT_OK(check("test", BSON("user" << "andy" << "userSource" << "test" <<
                                         "roles" << BSON_ARRAY("read"))));

        // Cannot have both "userSource" and "pwd"
        ASSERT_NOT_OK(check("test", BSON("user" << "andy" << "userSource" << "test2" <<
                                         "pwd" << "a" << "roles" << BSON_ARRAY("read"))));

        // Cannot have an otherDBRoles field except in the admin database.
        ASSERT_NOT_OK(check("test",
                            BSON("user" << "andy" << "userSource" << "test2" <<
                                 "roles" << BSON_ARRAY("read") <<
                                 "otherDBRoles" << BSON("test2" << BSON_ARRAY("readWrite")))));

        ASSERT_OK(check("admin",
                        BSON("user" << "andy" << "userSource" << "test2" <<
                             "roles" << BSON_ARRAY("read") <<
                             "otherDBRoles" << BSON("test2" << BSON_ARRAY("readWrite")))));

        // Must have "roles" to have "otherDBRoles".
        ASSERT_NOT_OK(check("admin",
                            BSON("user" << "andy" << "pwd" << "a" <<
                                 "otherDBRoles" << BSON("test2" << BSON_ARRAY("readWrite")))));

        ASSERT_OK(check("admin",
                        BSON("user" << "andy" << "pwd" << "a" <<
                             "roles" << BSONArrayBuilder().arr() <<
                             "otherDBRoles" << BSON("test2" << BSON_ARRAY("readWrite")))));

        // "otherDBRoles" may be empty.
        ASSERT_OK(check("admin",
                        BSON("user" << "andy" << "pwd" << "a" <<
                             "roles" << BSONArrayBuilder().arr() <<
                             "otherDBRoles" << BSONObjBuilder().obj())));

        // Cannot omit "roles" if "userSource" is present.
        ASSERT_NOT_OK(check("test", BSON("user" << "andy" << "userSource" << "test2")));

        // Cannot have both "roles" and "readOnly".
        ASSERT_NOT_OK(check("test", BSON("user" << "andy" << "pwd" << "a" << "readOnly" << 1 <<
                                         "roles" << BSON_ARRAY("read"))));

        // Roles must be strings, not empty.
        ASSERT_NOT_OK(check("test", BSON("user" << "andy" << "pwd" << "a" <<
                                         "roles" << BSON_ARRAY("read" << ""))));

        ASSERT_NOT_OK(check("test", BSON("user" << "andy" << "pwd" << "a" <<
                                         "roles" << BSON_ARRAY(1 << "read"))));

        // Multiple roles OK.
        ASSERT_OK(check("test", BSON("user" << "andy" << "pwd" << "a" <<
                                     "roles" << BSON_ARRAY("dbAdmin" << "read"))));

        // Empty roles list OK.
        ASSERT_OK(check("test", BSON("user" << "andy" << "pwd" << "a" <<
                                     "roles" << BSONArrayBuilder().arr())));
    }

    class AuthExternalStateImplictPriv : public AuthzSessionExternalStateMock {
    public:
        AuthExternalStateImplictPriv(AuthorizationManager* authzManager) :
            AuthzSessionExternalStateMock(authzManager) {}

        virtual bool _findUser(const string& usersNamespace,
                               const BSONObj& query,
                               BSONObj* result) const {

            NamespaceString nsstring(usersNamespace);
            std::string user = query[AuthorizationManager::USER_NAME_FIELD_NAME].String();
            std::string userSource;
            if (!query[AuthorizationManager::USER_SOURCE_FIELD_NAME].trueValue()) {
                userSource = nsstring.db;
            }
            else {
                userSource = query[AuthorizationManager::USER_SOURCE_FIELD_NAME].String();
            }
            *result = mapFindWithDefault(_privilegeDocs,
                                         std::make_pair(nsstring.db,
                                                        UserName(user, userSource)),
                                         BSON("invalid" << 1));
            return  !(*result)["invalid"].trueValue();
        }

        void addPrivilegeDocument(const string& dbname,
                                  const UserName& user,
                                  const BSONObj& doc) {

            ASSERT(_privilegeDocs.insert(std::make_pair(std::make_pair(dbname, user),
                                                        doc.getOwned())).second);
        }

    private:
        std::map<std::pair<std::string, UserName>, BSONObj > _privilegeDocs;
    };

    class ImplicitPriviligesTest : public ::mongo::unittest::Test {
    public:
        AuthExternalStateImplictPriv* state;
        scoped_ptr<AuthorizationSession> authzSession;
        scoped_ptr<AuthorizationManager> authzManager;

        void setUp() {
            authzManager.reset(new AuthorizationManager(new AuthzManagerExternalStateMock()));
            state = new AuthExternalStateImplictPriv(authzManager.get());
            authzSession.reset(new AuthorizationSession(state));
        }
    };

    TEST_F(ImplicitPriviligesTest, ImplicitAcquireFromSomeDatabases) {
        state->addPrivilegeDocument("test", UserName("andy", "test"),
                                    BSON("user" << "andy" <<
                                         "pwd" << "a" <<
                                         "roles" << BSON_ARRAY("readWrite")));
        state->addPrivilegeDocument("test2", UserName("andy", "test"),
                                    BSON("user" << "andy" <<
                                         "userSource" << "test" <<
                                         "roles" <<  BSON_ARRAY("read")));
        state->addPrivilegeDocument("admin", UserName("andy", "test"),
                                    BSON("user" << "andy" <<
                                         "userSource" << "test" <<
                                         "roles" << BSON_ARRAY("clusterAdmin") <<
                                         "otherDBRoles" << BSON("test3" << BSON_ARRAY("dbAdmin"))));

        ASSERT(!authzSession->checkAuthorization("test.foo", ActionType::find));
        ASSERT(!authzSession->checkAuthorization("test.foo", ActionType::insert));
        ASSERT(!authzSession->checkAuthorization("test.foo", ActionType::collMod));
        ASSERT(!authzSession->checkAuthorization("test2.foo", ActionType::find));
        ASSERT(!authzSession->checkAuthorization("test2.foo", ActionType::insert));
        ASSERT(!authzSession->checkAuthorization("test2.foo", ActionType::collMod));
        ASSERT(!authzSession->checkAuthorization("test3.foo", ActionType::find));
        ASSERT(!authzSession->checkAuthorization("test3.foo", ActionType::insert));
        ASSERT(!authzSession->checkAuthorization("test3.foo", ActionType::collMod));
        ASSERT(!authzSession->checkAuthorization("admin.foo", ActionType::find));
        ASSERT(!authzSession->checkAuthorization("admin.foo", ActionType::insert));
        ASSERT(!authzSession->checkAuthorization("admin.foo", ActionType::collMod));
        ASSERT(!authzSession->checkAuthorization("$SERVER", ActionType::shutdown));

        Principal* principal = new Principal(UserName("andy", "test"));
        principal->setImplicitPrivilegeAcquisition(true);
        authzSession->addAuthorizedPrincipal(principal);

        ASSERT(authzSession->checkAuthorization("test.foo", ActionType::find));
        ASSERT(authzSession->checkAuthorization("test.foo", ActionType::insert));
        ASSERT(!authzSession->checkAuthorization("test.foo", ActionType::collMod));
        ASSERT(authzSession->checkAuthorization("test2.foo", ActionType::find));
        ASSERT(!authzSession->checkAuthorization("test2.foo", ActionType::insert));
        ASSERT(!authzSession->checkAuthorization("test2.foo", ActionType::collMod));
        ASSERT(!authzSession->checkAuthorization("test3.foo", ActionType::find));
        ASSERT(!authzSession->checkAuthorization("test3.foo", ActionType::insert));
        ASSERT(authzSession->checkAuthorization("test3.foo", ActionType::collMod));
        ASSERT(!authzSession->checkAuthorization("admin.foo", ActionType::find));
        ASSERT(!authzSession->checkAuthorization("admin.foo", ActionType::insert));
        ASSERT(!authzSession->checkAuthorization("admin.foo", ActionType::collMod));
        ASSERT(authzSession->checkAuthorization("$SERVER", ActionType::shutdown));
    }

}  // namespace
}  // namespace mongo
