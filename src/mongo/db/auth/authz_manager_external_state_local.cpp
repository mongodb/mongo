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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authz_manager_external_state_local.h"

#include "mongo/base/status.h"
#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/element.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/address_restriction.h"
#include "mongo/db/auth/auth_options_gen.h"
#include "mongo/db/auth/privilege_parser.h"
#include "mongo/db/auth/user_document_parser.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_options.h"
#include "mongo/logv2/log.h"
#include "mongo/util/net/ssl_types.h"
#include "mongo/util/str.h"

namespace mongo {

using std::vector;
using ResolveRoleOption = AuthzManagerExternalStateLocal::ResolveRoleOption;

Status AuthzManagerExternalStateLocal::initialize(OperationContext* opCtx) {
    Status status = _initializeRoleGraph(opCtx);
    if (!status.isOK()) {
        if (status == ErrorCodes::GraphContainsCycle) {
            LOGV2_ERROR(23750,
                        "Cycle detected in admin.system.roles; role inheritance disabled. "
                        "Remove the listed cycle and any others to re-enable role inheritance",
                        "error"_attr = redact(status));
        } else {
            LOGV2_ERROR(23751,
                        "Could not generate role graph from admin.system.roles; "
                        "only system roles available",
                        "error"_attr = redact(status));
        }
    }

    _hasAnyPrivilegeDocuments.store(_checkHasAnyPrivilegeDocuments(opCtx));
    return Status::OK();
}

Status AuthzManagerExternalStateLocal::getStoredAuthorizationVersion(OperationContext* opCtx,
                                                                     int* outVersion) {
    BSONObj versionDoc;
    Status status = findOne(opCtx,
                            AuthorizationManager::versionCollectionNamespace,
                            AuthorizationManager::versionDocumentQuery,
                            &versionDoc);
    if (status.isOK()) {
        BSONElement versionElement = versionDoc[AuthorizationManager::schemaVersionFieldName];
        if (versionElement.isNumber()) {
            *outVersion = versionElement.numberInt();
            return Status::OK();
        } else if (versionElement.eoo()) {
            return Status(ErrorCodes::NoSuchKey,
                          str::stream() << "No " << AuthorizationManager::schemaVersionFieldName
                                        << " field in version document.");
        } else {
            return Status(ErrorCodes::TypeMismatch,
                          str::stream()
                              << "Could not determine schema version of authorization data.  "
                                 "Bad (non-numeric) type "
                              << typeName(versionElement.type()) << " (" << versionElement.type()
                              << ") for " << AuthorizationManager::schemaVersionFieldName
                              << " field in version document");
        }
    } else if (status == ErrorCodes::NoMatchingDocument) {
        *outVersion = AuthorizationManager::schemaVersion28SCRAM;
        return Status::OK();
    } else {
        return status;
    }
}

namespace {
void addRoleNameToObjectElement(mutablebson::Element object, const RoleName& role) {
    fassert(17153, object.appendString(AuthorizationManager::ROLE_NAME_FIELD_NAME, role.getRole()));
    fassert(17154, object.appendString(AuthorizationManager::ROLE_DB_FIELD_NAME, role.getDB()));
}

void addRoleNameObjectsToArrayElement(mutablebson::Element array, RoleNameIterator roles) {
    for (; roles.more(); roles.next()) {
        mutablebson::Element roleElement = array.getDocument().makeElementObject("");
        addRoleNameToObjectElement(roleElement, roles.get());
        fassert(17155, array.pushBack(roleElement));
    }
}

void addPrivilegeObjectsOrWarningsToArrayElement(mutablebson::Element privilegesElement,
                                                 mutablebson::Element warningsElement,
                                                 const PrivilegeVector& privileges) {
    std::string errmsg;
    for (size_t i = 0; i < privileges.size(); ++i) {
        ParsedPrivilege pp;
        if (ParsedPrivilege::privilegeToParsedPrivilege(privileges[i], &pp, &errmsg)) {
            fassert(17156, privilegesElement.appendObject("", pp.toBSON()));
        } else {
            fassert(17157,
                    warningsElement.appendString(
                        "",
                        std::string(str::stream() << "Skipped privileges on resource "
                                                  << privileges[i].getResourcePattern().toString()
                                                  << ". Reason: " << errmsg)));
        }
    }
}

void addAuthenticationRestrictionObjectsToArrayElement(
    mutablebson::Element restrictionsElement,
    const std::vector<SharedRestrictionDocument>& restrictions) {
    for (const auto& r : restrictions) {
        fassert(40560, restrictionsElement.appendArray("", r->toBSON()));
    }
}

void serializeResolvedRoles(BSONObjBuilder* user,
                            const AuthzManagerExternalState::ResolvedRoleData& data,
                            const BSONObj& roleDoc) {
    BSONArrayBuilder rolesBuilder(user->subarrayStart("inheritedRoles"));
    for (const auto& roleName : data.roles.get()) {
        roleName.serializeToBSON(&rolesBuilder);
    }
    rolesBuilder.doneFast();

    if (data.privileges) {
        BSONArrayBuilder privsBuilder(user->subarrayStart("inheritedPrivileges"));
        for (const auto& privilege : data.privileges.get()) {
            privsBuilder.append(privilege.toBSON());
        }
        privsBuilder.doneFast();
    }

    if (data.restrictions) {
        BSONArrayBuilder arBuilder(user->subarrayStart("inheritedAuthenticationRestrictions"));
        if (auto ar = roleDoc["authenticationRestrictions"];
            (ar.type() == Array) && (ar.Obj().nFields() > 0)) {
            arBuilder.append(ar);
        }
        if (auto ar = data.restrictions->toBSON(); ar.nFields() > 0) {
            arBuilder.append(ar);
        }
        arBuilder.doneFast();
    }
}

/**
 * Make sure the roleDoc as retreived from storage matches expectations for options.
 */
constexpr auto kRolesFieldName = "roles"_sd;
constexpr auto kPrivilegesFieldName = "privileges"_sd;
constexpr auto kAuthenticationRestrictionFieldName = "authenticationRestrictions"_sd;

std::vector<RoleName> filterAndMapRole(BSONObjBuilder* builder,
                                       BSONObj role,
                                       ResolveRoleOption option) {
    std::vector<RoleName> subRoles;
    bool sawRestrictions = false;

    for (const auto& elem : role) {
        if (elem.fieldNameStringData() == kRolesFieldName) {
            uassert(
                ErrorCodes::BadValue, "Invalid roles field, expected array", elem.type() == Array);
            for (const auto& roleName : elem.Obj()) {
                subRoles.push_back(RoleName::parseFromBSON(roleName));
            }
            if ((option & ResolveRoleOption::kRoles) == 0) {
                continue;
            }
        }

        if ((elem.fieldNameStringData() == kPrivilegesFieldName) &&
            ((option & ResolveRoleOption::kPrivileges) == 0)) {
            continue;
        }

        if (elem.fieldNameStringData() == kAuthenticationRestrictionFieldName) {
            sawRestrictions = true;
            if (option & ResolveRoleOption::kRestrictions) {
                BSONArrayBuilder arBuilder(
                    builder->subarrayStart(kAuthenticationRestrictionFieldName));
                arBuilder.append(elem);
                arBuilder.doneFast();
            }
            continue;
        }

        builder->append(elem);
    }

    if (!sawRestrictions && (option & ResolveRoleOption::kRestrictions)) {
        builder->append(kAuthenticationRestrictionFieldName, BSONArray());
    }

    return subRoles;
}

ResolveRoleOption makeResolveRoleOption(PrivilegeFormat showPrivileges,
                                        AuthenticationRestrictionsFormat showRestrictions) {
    auto option = ResolveRoleOption::kRoles;
    if (showPrivileges != PrivilegeFormat::kOmit) {
        option = static_cast<ResolveRoleOption>(option | ResolveRoleOption::kPrivileges);
    }
    if (showRestrictions != AuthenticationRestrictionsFormat::kOmit) {
        option = static_cast<ResolveRoleOption>(option | ResolveRoleOption::kRestrictions);
    }

    return option;
}

}  // namespace

bool AuthzManagerExternalStateLocal::_checkHasAnyPrivilegeDocuments(OperationContext* opCtx) {
    BSONObj userBSONObj;
    Status statusFindUsers =
        findOne(opCtx, AuthorizationManager::usersCollectionNamespace, BSONObj(), &userBSONObj);

    // If we were unable to complete the query,
    // it's best to assume that there _are_ privilege documents.
    if (statusFindUsers != ErrorCodes::NoMatchingDocument) {
        return true;
    }
    Status statusFindRoles =
        findOne(opCtx, AuthorizationManager::rolesCollectionNamespace, BSONObj(), &userBSONObj);
    return statusFindRoles != ErrorCodes::NoMatchingDocument;
}

Status AuthzManagerExternalStateLocal::getUserDescription(OperationContext* opCtx,
                                                          const UserRequest& userReq,
                                                          BSONObj* result) {
    Status status = Status::OK();
    const UserName& userName = userReq.name;

    if (!userReq.roles) {
        status = _getUserDocument(opCtx, userName, result);
        if (!status.isOK())
            return status;
    } else {
        // We are able to artifically construct the external user from the request
        BSONArrayBuilder userRoles;
        for (const RoleName& role : *(userReq.roles)) {
            userRoles << BSON("role" << role.getRole() << "db" << role.getDB());
        }
        *result = BSON("_id" << userName.getUser() << "user" << userName.getUser() << "db"
                             << userName.getDB() << "credentials" << BSON("external" << true)
                             << "roles" << userRoles.arr());
    }

    BSONElement directRolesElement;
    status = bsonExtractTypedField(*result, "roles", Array, &directRolesElement);
    if (!status.isOK())
        return status;
    std::vector<RoleName> directRoles;
    status =
        V2UserDocumentParser::parseRoleVector(BSONArray(directRolesElement.Obj()), &directRoles);
    if (!status.isOK())
        return status;

    mutablebson::Document resultDoc(*result, mutablebson::Document::kInPlaceDisabled);
    resolveUserRoles(&resultDoc, directRoles);
    *result = resultDoc.getObject();

    return Status::OK();
}

void AuthzManagerExternalStateLocal::resolveUserRoles(mutablebson::Document* userDoc,
                                                      const std::vector<RoleName>& directRoles) {
    stdx::unordered_set<RoleName> indirectRoles;
    PrivilegeVector allPrivileges;
    std::vector<SharedRestrictionDocument> allAuthenticationRestrictions;
    bool isRoleGraphConsistent = false;

    {
        stdx::lock_guard<Latch> lk(_roleGraphMutex);
        isRoleGraphConsistent = _roleGraphState == roleGraphStateConsistent;
        for (const auto& role : directRoles) {
            indirectRoles.insert(role);
            if (isRoleGraphConsistent) {
                for (RoleNameIterator subordinates = _roleGraph.getIndirectSubordinates(role);
                     subordinates.more();
                     subordinates.next()) {
                    indirectRoles.insert(subordinates.get());
                }
            }

            const auto& currentPrivileges = isRoleGraphConsistent
                ? _roleGraph.getAllPrivileges(role)
                : _roleGraph.getDirectPrivileges(role);
            for (const auto& priv : currentPrivileges) {
                Privilege::addPrivilegeToPrivilegeVector(&allPrivileges, priv);
            }

            if (isRoleGraphConsistent) {
                const auto& currentAuthenticationRestrictions =
                    _roleGraph.getAllAuthenticationRestrictions(role);
                allAuthenticationRestrictions.insert(allAuthenticationRestrictions.end(),
                                                     currentAuthenticationRestrictions.begin(),
                                                     currentAuthenticationRestrictions.end());
            } else {
                const auto& dar = _roleGraph.getDirectAuthenticationRestrictions(role);
                if (dar.get()) {
                    allAuthenticationRestrictions.push_back(dar);
                }
            }
        }
    }

    auto warningsElement = userDoc->makeElementArray("warnings");

    auto inheritedRolesElement = userDoc->makeElementArray("inheritedRoles");
    fassert(17159, userDoc->root().pushBack(inheritedRolesElement));
    addRoleNameObjectsToArrayElement(inheritedRolesElement,
                                     makeRoleNameIteratorForContainer(indirectRoles));

    auto privilegesElement = userDoc->makeElementArray("inheritedPrivileges");
    fassert(17158, userDoc->root().pushBack(privilegesElement));
    addPrivilegeObjectsOrWarningsToArrayElement(privilegesElement, warningsElement, allPrivileges);

    auto inheritedAuthenticationRestrictionsElement =
        userDoc->makeElementArray("inheritedAuthenticationRestrictions");
    fassert(40558, userDoc->root().pushBack(inheritedAuthenticationRestrictionsElement));
    addAuthenticationRestrictionObjectsToArrayElement(inheritedAuthenticationRestrictionsElement,
                                                      allAuthenticationRestrictions);

    if (!mutablebson::findFirstChildNamed(userDoc->root(), "authenticationRestrictions").ok()) {
        auto authenticationRestrictionsElement =
            userDoc->makeElementArray("authenticationRestrictions");
        fassert(40572, userDoc->root().pushBack(authenticationRestrictionsElement));
    }

    if (!isRoleGraphConsistent) {
        fassert(17160,
                warningsElement.appendString(
                    "", "Role graph inconsistent, only direct privileges available."));
    }

    if (warningsElement.hasChildren()) {
        fassert(17161, userDoc->root().pushBack(warningsElement));
    }
}

Status AuthzManagerExternalStateLocal::_getUserDocument(OperationContext* opCtx,
                                                        const UserName& userName,
                                                        BSONObj* userDoc) {
    Status status = findOne(opCtx,
                            AuthorizationManager::usersCollectionNamespace,
                            BSON(AuthorizationManager::USER_NAME_FIELD_NAME
                                 << userName.getUser() << AuthorizationManager::USER_DB_FIELD_NAME
                                 << userName.getDB()),
                            userDoc);

    if (status == ErrorCodes::NoMatchingDocument) {
        status = Status(ErrorCodes::UserNotFound,
                        str::stream() << "Could not find user \"" << userName.getUser()
                                      << "\" for db \"" << userName.getDB() << "\"");
    }
    return status;
}

Status AuthzManagerExternalStateLocal::rolesExist(OperationContext* opCtx,
                                                  const std::vector<RoleName>& roleNames) {
    // Perform DB queries for user-defined roles (skipping builtin roles).
    stdx::unordered_set<RoleName> unknownRoles;
    for (const auto& roleName : roleNames) {
        if (!RoleGraph::isBuiltinRole(roleName) &&
            !hasOne(opCtx, AuthorizationManager::rolesCollectionNamespace, roleName.toBSON())) {
            unknownRoles.insert(roleName);
        }
    }

    // If anything remains, raise it as an unknown role error.
    if (!unknownRoles.empty()) {
        return makeRoleNotFoundStatus(unknownRoles);
    }

    return Status::OK();
}

using ResolvedRoleData = AuthzManagerExternalState::ResolvedRoleData;
StatusWith<ResolvedRoleData> AuthzManagerExternalStateLocal::resolveRoles(
    OperationContext* opCtx, const std::vector<RoleName>& roleNames, ResolveRoleOption option) try {
    using RoleNameSet = typename decltype(ResolvedRoleData::roles)::value_type;
    const bool processRoles = option & ResolveRoleOption::kRoles;
    const bool processPrivs = option & ResolveRoleOption::kPrivileges;
    const bool processRests = option & ResolveRoleOption::kRestrictions;
    const bool walkIndirect = (option & ResolveRoleOption::kDirectOnly) == 0;

    RoleNameSet inheritedRoles;
    PrivilegeVector inheritedPrivileges;
    RestrictionDocuments::sequence_type inheritedRestrictions;

    RoleNameSet frontier(roleNames.cbegin(), roleNames.cend());
    RoleNameSet visited;
    while (!frontier.empty()) {
        RoleNameSet nextFrontier;
        for (const auto& role : frontier) {
            visited.insert(role);

            if (RoleGraph::isBuiltinRole(role)) {
                if (processPrivs) {
                    RoleGraph::addPrivilegesForBuiltinRole(role, &inheritedPrivileges);
                }
                continue;
            }

            BSONObj roleDoc;
            auto status = findOne(
                opCtx, AuthorizationManager::rolesCollectionNamespace, role.toBSON(), &roleDoc);
            if (!status.isOK()) {
                if (status.code() == ErrorCodes::NoMatchingDocument) {
                    return {ErrorCodes::RoleNotFound,
                            str::stream() << "Role '" << role.getFullName() << "' does not exist"};
                }
                return status;
            }

            BSONElement elem;
            if ((processRoles || walkIndirect) && (elem = roleDoc["roles"])) {
                if (elem.type() != Array) {
                    return {ErrorCodes::BadValue,
                            str::stream()
                                << "Invalid 'roles' field in role document '" << role.getFullName()
                                << "', expected an array but found " << typeName(elem.type())};
                }
                for (const auto& subroleElem : elem.Obj()) {
                    auto subrole = RoleName::parseFromBSON(subroleElem);
                    if (visited.count(subrole) || nextFrontier.count(subrole)) {
                        continue;
                    }
                    if (walkIndirect) {
                        nextFrontier.insert(subrole);
                    }
                    if (processRoles) {
                        inheritedRoles.insert(std::move(subrole));
                    }
                }
            }

            if (processPrivs && (elem = roleDoc["privileges"])) {
                if (elem.type() != Array) {
                    return {ErrorCodes::BadValue,
                            str::stream() << "Invalid 'privileges' field in role document '"
                                          << role.getFullName() << "'"};
                }
                for (const auto& privElem : elem.Obj()) {
                    auto priv = Privilege::fromBSON(privElem);
                    Privilege::addPrivilegeToPrivilegeVector(&inheritedPrivileges, priv);
                }
            }

            if (processRests && (elem = roleDoc["authenticationRestrictions"])) {
                if (elem.type() != Array) {
                    return {ErrorCodes::BadValue,
                            str::stream()
                                << "Invalid 'authenticationRestrictions' field in role document '"
                                << role.getFullName() << "'"};
                }
                inheritedRestrictions.push_back(
                    uassertStatusOK(parseAuthenticationRestriction(BSONArray(elem.Obj()))));
            }
        }
        frontier = std::move(nextFrontier);
    }

    ResolvedRoleData ret;
    if (processRoles) {
        ret.roles = std::move(inheritedRoles);
    }
    if (processPrivs) {
        ret.privileges = std::move(inheritedPrivileges);
    }
    if (processRests) {
        ret.restrictions = RestrictionDocuments(std::move(inheritedRestrictions));
    }

    return ret;
} catch (const AssertionException& ex) {
    return ex.toStatus();
}

Status AuthzManagerExternalStateLocal::getRolesDescription(
    OperationContext* opCtx,
    const std::vector<RoleName>& roleNames,
    PrivilegeFormat showPrivileges,
    AuthenticationRestrictionsFormat showRestrictions,
    BSONObj* result) {
    auto option = makeResolveRoleOption(showPrivileges, showRestrictions);

    if (showPrivileges == PrivilegeFormat::kShowAsUserFragment) {
        BSONObjBuilder fragment;

        BSONArrayBuilder rolesBuilder(fragment.subarrayStart("roles"));
        for (const auto& roleName : roleNames) {
            roleName.serializeToBSON(&rolesBuilder);
        }
        rolesBuilder.doneFast();

        auto swData = resolveRoles(opCtx, roleNames, option);
        if (!swData.isOK()) {
            return swData.getStatus();
        }
        auto data = std::move(swData.getValue());
        data.roles->insert(roleNames.cbegin(), roleNames.cend());
        serializeResolvedRoles(&fragment, data, BSONObj());
        *result = fragment.obj();
        return Status::OK();
    }

    BSONArrayBuilder rolesBuilder;
    for (const RoleName& role : roleNames) {
        try {
            BSONObj roleDoc;
            auto status = findOne(
                opCtx, AuthorizationManager::rolesCollectionNamespace, role.toBSON(), &roleDoc);
            if (!status.isOK()) {
                if (status.code() == ErrorCodes::NoMatchingDocument) {
                    continue;
                }
                uassertStatusOK(status);  // throws
            }

            BSONObjBuilder roleBuilder(rolesBuilder.subobjStart());
            auto subRoles = filterAndMapRole(&roleBuilder, roleDoc, option);
            auto data = uassertStatusOK(resolveRoles(opCtx, subRoles, option));
            data.roles->insert(subRoles.cbegin(), subRoles.cend());
            serializeResolvedRoles(&roleBuilder, data, roleDoc);

            roleBuilder.doneFast();
        } catch (const AssertionException& ex) {
            return {ex.code(),
                    str::stream() << "Failed fetching role '" << role.getFullName()
                                  << "': " << ex.reason()};
        }
    }

    *result = rolesBuilder.arr();
    return Status::OK();
}

Status AuthzManagerExternalStateLocal::getRoleDescriptionsForDB(
    OperationContext* opCtx,
    StringData dbname,
    PrivilegeFormat showPrivileges,
    AuthenticationRestrictionsFormat showRestrictions,
    bool showBuiltinRoles,
    BSONArrayBuilder* result) {
    auto option = makeResolveRoleOption(showPrivileges, showRestrictions);

    if (showPrivileges == PrivilegeFormat::kShowAsUserFragment) {
        return {ErrorCodes::IllegalOperation,
                "Cannot get user fragment for all roles in a database"};
    }

    if (showBuiltinRoles) {
        for (const auto& roleName : RoleGraph::getBuiltinRoleNamesForDB(dbname)) {
            BSONObjBuilder roleBuilder(result->subobjStart());

            roleBuilder.append(AuthorizationManager::ROLE_NAME_FIELD_NAME, roleName.getRole());
            roleBuilder.append(AuthorizationManager::ROLE_DB_FIELD_NAME, roleName.getDB());
            roleBuilder.append("isBuiltin", true);

            roleBuilder.append("roles", BSONArray());
            roleBuilder.append("inheritedRoles", BSONArray());

            if (showPrivileges == PrivilegeFormat::kShowSeparate) {
                BSONArrayBuilder privsBuilder(roleBuilder.subarrayStart("privileges"));
                PrivilegeVector privs;
                RoleGraph::addPrivilegesForBuiltinRole(roleName, &privs);
                for (const auto& privilege : privs) {
                    privsBuilder.append(privilege.toBSON());
                }
                privsBuilder.doneFast();

                // Builtin roles have identival privs/inheritedPrivs
                BSONArrayBuilder ipBuilder(roleBuilder.subarrayStart("inheritedPrivileges"));
                for (const auto& privilege : privs) {
                    ipBuilder.append(privilege.toBSON());
                }
                ipBuilder.doneFast();
            }

            if (showRestrictions == AuthenticationRestrictionsFormat::kShow) {
                roleBuilder.append("authenticationRestrictions", BSONArray());
                roleBuilder.append("inheritedAuthenticationRestrictions", BSONArray());
            }

            roleBuilder.doneFast();
        }
    }

    return query(opCtx,
                 AuthorizationManager::rolesCollectionNamespace,
                 BSON(AuthorizationManager::ROLE_DB_FIELD_NAME << dbname),
                 BSONObj(),
                 [&](const BSONObj& roleDoc) {
                     try {
                         BSONObjBuilder roleBuilder(result->subobjStart());

                         auto subRoles = filterAndMapRole(&roleBuilder, roleDoc, option);
                         roleBuilder.append("isBuiltin", false);
                         auto data = uassertStatusOK(resolveRoles(opCtx, subRoles, option));
                         data.roles->insert(subRoles.cbegin(), subRoles.cend());
                         serializeResolvedRoles(&roleBuilder, data, roleDoc);
                         roleBuilder.doneFast();
                         return Status::OK();
                     } catch (const AssertionException& ex) {
                         return ex.toStatus();
                     }
                 });
}

namespace {

/**
 * Adds the role described in "doc" to "roleGraph".  If the role cannot be added, due to
 * some error in "doc", logs a warning.
 */
void addRoleFromDocumentOrWarn(RoleGraph* roleGraph, const BSONObj& doc) {
    Status status = roleGraph->addRoleFromDocument(doc);
    if (!status.isOK()) {
        LOGV2_WARNING(23747,
                      "Skipping invalid admin.system.roles document while calculating privileges"
                      " for user-defined roles",
                      "error"_attr = redact(status),
                      "doc"_attr = redact(doc));
    }
}


}  // namespace

Status AuthzManagerExternalStateLocal::_initializeRoleGraph(OperationContext* opCtx) {
    stdx::lock_guard<Latch> lkInitialzeRoleGraph(_roleGraphMutex);

    _roleGraphState = roleGraphStateInitial;
    _roleGraph = RoleGraph();

    RoleGraph newRoleGraph;
    Status status = query(
        opCtx,
        AuthorizationManager::rolesCollectionNamespace,
        BSONObj(),
        BSONObj(),
        [p = &newRoleGraph](const BSONObj& doc) { return addRoleFromDocumentOrWarn(p, doc); });
    if (!status.isOK())
        return status;

    status = newRoleGraph.recomputePrivilegeData();

    RoleGraphState newState;
    if (status == ErrorCodes::GraphContainsCycle) {
        LOGV2_ERROR(23752,
                    "Inconsistent role graph during authorization manager initialization. Only "
                    "direct privileges available.",
                    "error"_attr = redact(status));
        newState = roleGraphStateHasCycle;
        status = Status::OK();
    } else if (status.isOK()) {
        newState = roleGraphStateConsistent;
    } else {
        newState = roleGraphStateInitial;
    }

    if (status.isOK()) {
        _roleGraph = std::move(newRoleGraph);
        _roleGraphState = std::move(newState);
    }
    return status;
}

class AuthzManagerExternalStateLocal::AuthzManagerLogOpHandler : public RecoveryUnit::Change {
public:
    // None of the parameters below (except opCtx and externalState) need to live longer than the
    // instantiations of this class
    AuthzManagerLogOpHandler(OperationContext* opCtx,
                             AuthorizationManagerImpl* authzManager,
                             AuthzManagerExternalStateLocal* externalState,
                             const char* op,
                             const NamespaceString& nss,
                             const BSONObj& o,
                             const BSONObj* o2)
        : _opCtx(opCtx),
          _authzManager(authzManager),
          _externalState(externalState),
          _op(op),
          _nss(nss),
          _o(o.getOwned()),
          _o2(o2 ? boost::optional<BSONObj>(o2->getOwned()) : boost::none) {

        _invalidateRelevantCacheData();
    }

    void commit(boost::optional<Timestamp> timestamp) final {
        const bool isRolesColl = _nss == AuthorizationManager::rolesCollectionNamespace;
        const bool isUsersColl = _nss == AuthorizationManager::usersCollectionNamespace;
        const bool isAdminComm = _nss == AuthorizationManager::adminCommandNamespace;
        if (isRolesColl || isAdminComm) {
            _refreshRoleGraph();
        }
        if ((isRolesColl || isUsersColl) && (_op == "i")) {
            _externalState->setHasAnyPrivilegeDocuments();
        }
    }

    void rollback() final {}

private:
    // Updates to users in the oplog are done by matching on the _id, which will always have the
    // form "<dbname>.<username>".  This function extracts the UserName from that string.
    static StatusWith<UserName> extractUserNameFromIdString(StringData idstr) {
        size_t splitPoint = idstr.find('.');
        if (splitPoint == std::string::npos) {
            return StatusWith<UserName>(ErrorCodes::FailedToParse,
                                        str::stream()
                                            << "_id entries for user documents must be of "
                                               "the form <dbname>.<username>.  Found: "
                                            << idstr);
        }
        return StatusWith<UserName>(
            UserName(idstr.substr(splitPoint + 1), idstr.substr(0, splitPoint)));
    }


    void _refreshRoleGraph() {
        stdx::lock_guard<Latch> lk(_externalState->_roleGraphMutex);
        Status status = _externalState->_roleGraph.handleLogOp(
            _opCtx, _op.c_str(), _nss, _o, _o2 ? &*_o2 : nullptr);

        if (status == ErrorCodes::OplogOperationUnsupported) {
            _externalState->_roleGraph = RoleGraph();
            _externalState->_roleGraphState = _externalState->roleGraphStateInitial;
            BSONObjBuilder oplogEntryBuilder;
            oplogEntryBuilder << "op" << _op << "ns" << _nss.ns() << "o" << _o;
            if (_o2) {
                oplogEntryBuilder << "o2" << *_o2;
            }
            LOGV2_ERROR(23753,
                        "Unsupported modification to roles collection in oplog; "
                        "restart this process to reenable user-defined roles",
                        "error"_attr = redact(status),
                        "entry"_attr = redact(oplogEntryBuilder.done()));
            // If a setParameter is enabled, this condition is fatal.
            fassert(51152, !roleGraphInvalidationIsFatal);
        } else if (!status.isOK()) {
            LOGV2_WARNING(23748,
                          "Skipping bad update to roles collection in oplog",
                          "error"_attr = redact(status),
                          "op"_attr = redact(_op));
        }
        status = _externalState->_roleGraph.recomputePrivilegeData();
        if (status == ErrorCodes::GraphContainsCycle) {
            _externalState->_roleGraphState = _externalState->roleGraphStateHasCycle;
            LOGV2_ERROR(23754,
                        "Inconsistent role graph during authorization manager initialization. "
                        "Only direct privileges available after applying oplog entry",
                        "error"_attr = redact(status),
                        "op"_attr = redact(_op));
        } else {
            fassert(17183, status);
            _externalState->_roleGraphState = _externalState->roleGraphStateConsistent;
        }
    }

    void _invalidateRelevantCacheData() {
        if (_nss == AuthorizationManager::rolesCollectionNamespace ||
            _nss == AuthorizationManager::versionCollectionNamespace) {
            _authzManager->invalidateUserCache(_opCtx);
            return;
        }

        if (_op == "i" || _op == "d" || _op == "u") {
            // If you got into this function isAuthzNamespace() must have returned true, and we've
            // already checked that it's not the roles or version collection.
            invariant(_nss == AuthorizationManager::usersCollectionNamespace);

            StatusWith<UserName> userName = (_op == "u")
                ? extractUserNameFromIdString((*_o2)["_id"].str())
                : extractUserNameFromIdString(_o["_id"].str());

            if (!userName.isOK()) {
                LOGV2_WARNING(23749,
                              "Invalidating user cache based on user being updated failed, will "
                              "invalidate the entire cache instead",
                              "user"_attr = userName.getStatus());
                _authzManager->invalidateUserCache(_opCtx);
                return;
            }
            _authzManager->invalidateUserByName(_opCtx, userName.getValue());
        } else {
            _authzManager->invalidateUserCache(_opCtx);
        }
    }


    OperationContext* _opCtx;
    AuthorizationManagerImpl* _authzManager;
    AuthzManagerExternalStateLocal* _externalState;
    const std::string _op;
    const NamespaceString _nss;
    const BSONObj _o;
    const boost::optional<BSONObj> _o2;
};

void AuthzManagerExternalStateLocal::logOp(OperationContext* opCtx,
                                           AuthorizationManagerImpl* authzManager,
                                           const char* op,
                                           const NamespaceString& nss,
                                           const BSONObj& o,
                                           const BSONObj* o2) {
    if (nss == AuthorizationManager::rolesCollectionNamespace ||
        nss == AuthorizationManager::versionCollectionNamespace ||
        nss == AuthorizationManager::usersCollectionNamespace ||
        nss == AuthorizationManager::adminCommandNamespace) {

        auto change =
            std::make_unique<AuthzManagerLogOpHandler>(opCtx, authzManager, this, op, nss, o, o2);
        // AuthzManagerExternalState's logOp method registers a RecoveryUnit::Change
        // and to do so we need to have begun a UnitOfWork
        WriteUnitOfWork wuow(opCtx);

        opCtx->recoveryUnit()->registerChange(std::move(change));

        wuow.commit();
    }
}

}  // namespace mongo
