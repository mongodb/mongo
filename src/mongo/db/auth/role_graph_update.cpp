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

#include "mongo/platform/basic.h"

#include "mongo/base/status.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/element.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/address_restriction.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/restriction_set.h"
#include "mongo/db/auth/role_graph.h"
#include "mongo/db/auth/user_management_commands_parser.h"
#include "mongo/db/update/update_driver.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

namespace {

/**
 * Structure representing information parsed out of a role document.
 */
struct RoleInfo {
    RoleName name;
    std::vector<RoleName> roles;
    PrivilegeVector privileges;
    std::shared_ptr<RestrictionDocument<>> restrictions;
};

/**
 * Parses the role name out of a BSON document.
 */
Status parseRoleNameFromDocument(const BSONObj& doc, RoleName* name) {
    BSONElement nameElement;
    BSONElement sourceElement;
    Status status = bsonExtractTypedField(
        doc, AuthorizationManager::ROLE_NAME_FIELD_NAME, String, &nameElement);
    if (!status.isOK())
        return status;
    status = bsonExtractTypedField(
        doc, AuthorizationManager::ROLE_DB_FIELD_NAME, String, &sourceElement);
    if (!status.isOK())
        return status;
    *name = RoleName(nameElement.valueStringData(), sourceElement.valueStringData());
    return status;
}

/**
 * Checks whether the given "roleName" corresponds with the given _id field.
 * In admin.system.roles, documents with role name "role@db" must have _id
 * "db.role".
 *
 * Returns Status::OK if the two values are compatible.
 */
Status checkIdMatchesRoleName(const BSONElement& idElement, const RoleName& roleName) {
    if (idElement.type() != String) {
        return Status(ErrorCodes::TypeMismatch, "Role document _id fields must be strings.");
    }
    StringData idField = idElement.valueStringData();
    size_t firstDot = idField.find('.');
    if (firstDot == std::string::npos || idField.substr(0, firstDot) != roleName.getDB() ||
        idField.substr(firstDot + 1) != roleName.getRole()) {
        return Status(ErrorCodes::FailedToParse,
                      mongoutils::str::stream()
                          << "Role document _id fields must be encoded as the string "
                             "dbname.rolename.  Found "
                          << idField
                          << " for "
                          << roleName.getFullName());
    }
    return Status::OK();
}

/**
 * Parses "idElement" to extract the role name, according to the "dbname.role" convention
 * used for admin.system.roles documents.
 */
Status getRoleNameFromIdField(const BSONElement& idElement, RoleName* roleName) {
    if (idElement.type() != String) {
        return Status(ErrorCodes::TypeMismatch, "Role document _id fields must be strings.");
    }
    StringData idField = idElement.valueStringData();
    size_t dotPos = idField.find('.');
    if (dotPos == std::string::npos) {
        return Status(ErrorCodes::BadValue,
                      "Role document _id fields must have the form dbname.rolename");
    }
    *roleName = RoleName(idField.substr(dotPos + 1), idField.substr(0, dotPos));
    return Status::OK();
}

/**
 * Parses information about authenticationRestrictions from a BSON document
 */
Status parseAuthenticationRestrictions(const BSONElement& elem, RoleInfo* role) {
    if (elem.eoo()) {
        return Status::OK();
    }

    if (elem.type() != Array) {
        return Status(ErrorCodes::TypeMismatch,
                      "'authenticationRestricitons' field must be an array");
    }

    auto restrictions = parseAuthenticationRestriction(BSONArray(elem.Obj()));
    if (!restrictions.isOK()) {
        return restrictions.getStatus();
    }
    role->restrictions = std::move(restrictions.getValue());
    return Status::OK();
}

/**
 * Parses information about a role from a BSON document.
 */
Status parseRoleFromDocument(const BSONObj& doc, RoleInfo* role) {
    BSONElement rolesElement;
    Status status = parseRoleNameFromDocument(doc, &role->name);
    if (!status.isOK())
        return status;
    status = checkIdMatchesRoleName(doc["_id"], role->name);
    if (!status.isOK())
        return status;
    status = bsonExtractTypedField(doc, "roles", Array, &rolesElement);
    if (!status.isOK())
        return status;
    BSONForEach(singleRoleElement, rolesElement.Obj()) {
        if (singleRoleElement.type() != Object) {
            return Status(ErrorCodes::TypeMismatch, "Elements of roles array must be objects.");
        }
        RoleName possessedRoleName;
        status = parseRoleNameFromDocument(singleRoleElement.Obj(), &possessedRoleName);
        if (!status.isOK())
            return status;
        role->roles.push_back(possessedRoleName);
    }

    status = parseAuthenticationRestrictions(doc["authenticationRestrictions"], role);
    if (!status.isOK()) {
        return status;
    }

    BSONElement privilegesElement;
    status = bsonExtractTypedField(doc, "privileges", Array, &privilegesElement);
    if (!status.isOK())
        return status;
    status =
        auth::parseAndValidatePrivilegeArray(BSONArray(privilegesElement.Obj()), &role->privileges);
    return status;
}

/**
 * Updates roleGraph for an insert-type oplog operation on admin.system.roles.
 */
Status handleOplogInsert(RoleGraph* roleGraph, const BSONObj& insertedObj) {
    RoleInfo role;
    Status status = parseRoleFromDocument(insertedObj, &role);
    if (!status.isOK())
        return status;
    status = roleGraph->replaceRole(role.name, role.roles, role.privileges, role.restrictions);
    return status;
}

/**
 * Updates roleGraph for an update-type oplog operation on admin.system.roles.
 *
 * Treats all updates as upserts.
 */
Status handleOplogUpdate(OperationContext* opCtx,
                         RoleGraph* roleGraph,
                         const BSONObj& updatePattern,
                         const BSONObj& queryPattern) {
    RoleName roleToUpdate;
    Status status = getRoleNameFromIdField(queryPattern["_id"], &roleToUpdate);
    if (!status.isOK())
        return status;

    UpdateDriver::Options updateOptions;
    UpdateDriver driver(updateOptions);

    // Oplog updates do not have array filters.
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFilters;
    status = driver.parse(updatePattern, arrayFilters);
    if (!status.isOK())
        return status;

    mutablebson::Document roleDocument;
    status = AuthorizationManager::getBSONForRole(roleGraph, roleToUpdate, roleDocument.root());
    if (status == ErrorCodes::RoleNotFound) {
        // The query pattern will only contain _id, no other immutable fields are present
        const FieldRef idFieldRef("_id");
        FieldRefSet immutablePaths;
        invariant(immutablePaths.insert(&idFieldRef));
        status = driver.populateDocumentWithQueryFields(
            opCtx, queryPattern, immutablePaths, roleDocument);
    }
    if (!status.isOK())
        return status;

    // The original document can be empty because it is only needed for validation of immutable
    // paths.
    const BSONObj emptyOriginal;
    const bool validateForStorage = false;
    const FieldRefSet emptyImmutablePaths;
    status = driver.update(
        StringData(), emptyOriginal, &roleDocument, validateForStorage, emptyImmutablePaths);
    if (!status.isOK())
        return status;

    // Now use the updated document to totally replace the role in the graph!
    RoleInfo role;
    status = parseRoleFromDocument(roleDocument.getObject(), &role);
    if (!status.isOK())
        return status;
    status = roleGraph->replaceRole(role.name, role.roles, role.privileges, role.restrictions);

    return status;
}

/**
 * Updates roleGraph for a delete-type oplog operation on admin.system.roles.
 */
Status handleOplogDelete(RoleGraph* roleGraph, const BSONObj& deletePattern) {
    RoleName roleToDelete;
    Status status = getRoleNameFromIdField(deletePattern["_id"], &roleToDelete);
    if (!status.isOK())
        return status;
    status = roleGraph->deleteRole(roleToDelete);
    if (ErrorCodes::RoleNotFound == status) {
        // Double-delete can happen in oplog application.
        status = Status::OK();
    }
    return status;
}

/**
 * Updates roleGraph for command-type oplog operations on the admin database.
 */
Status handleOplogCommand(RoleGraph* roleGraph, const BSONObj& cmdObj) {
    const NamespaceString& rolesCollectionNamespace =
        AuthorizationManager::rolesCollectionNamespace;
    const StringData cmdName(cmdObj.firstElement().fieldNameStringData());
    if (cmdName == "applyOps") {
        // Operations applied by applyOps will be passed into RoleGraph::handleOplog() by the
        // implementation of applyOps itself.
        return Status::OK();
    }
    if (cmdName == "create") {
        return Status::OK();
    }
    if (cmdName == "drop") {
        if (cmdObj.firstElement().str() == rolesCollectionNamespace.coll()) {
            *roleGraph = RoleGraph();
        }
        return Status::OK();
    }
    if (cmdName == "dropDatabase") {
        *roleGraph = RoleGraph();
        return Status::OK();
    }
    if (cmdName == "renameCollection") {
        if (cmdObj.firstElement().str() == rolesCollectionNamespace.ns()) {
            *roleGraph = RoleGraph();
            return Status::OK();
        }
        if (cmdObj["to"].str() == rolesCollectionNamespace.ns()) {
            *roleGraph = RoleGraph();
            return Status(ErrorCodes::OplogOperationUnsupported,
                          "Renaming into admin.system.roles produces inconsistent state; "
                          "must resynchronize role graph.");
        }
        return Status::OK();
    }
    if (cmdName == "dropIndexes" || cmdName == "deleteIndexes") {
        return Status::OK();
    }
    if ((cmdName == "collMod" || cmdName == "emptycapped") &&
        cmdObj.firstElement().str() != rolesCollectionNamespace.coll()) {
        // We don't care about these if they're not on the roles collection.
        return Status::OK();
    }
    //  No other commands expected.  Warn.
    return Status(ErrorCodes::OplogOperationUnsupported, "Unsupported oplog operation");
}
}  // namespace

Status RoleGraph::addRoleFromDocument(const BSONObj& doc) {
    RoleInfo role;
    Status status = parseRoleFromDocument(doc, &role);
    if (!status.isOK())
        return status;
    status = replaceRole(role.name, role.roles, role.privileges, role.restrictions);
    return status;
}

Status RoleGraph::handleLogOp(OperationContext* opCtx,
                              const char* op,
                              const NamespaceString& ns,
                              const BSONObj& o,
                              const BSONObj* o2) {
    if (op == "db"_sd)
        return Status::OK();
    if (op[0] == '\0' || op[1] != '\0') {
        return Status(ErrorCodes::BadValue,
                      mongoutils::str::stream() << "Unrecognized \"op\" field value \"" << op
                                                << '"');
    }

    if (ns.db() != AuthorizationManager::rolesCollectionNamespace.db())
        return Status::OK();

    if (ns.isCommand()) {
        if (*op == 'c') {
            return handleOplogCommand(this, o);
        } else {
            return Status(ErrorCodes::BadValue, "Non-command oplog entry on admin.$cmd namespace");
        }
    }

    if (ns.coll() != AuthorizationManager::rolesCollectionNamespace.coll())
        return Status::OK();

    switch (*op) {
        case 'i':
            return handleOplogInsert(this, o);
        case 'u':
            if (!o2) {
                return Status(ErrorCodes::InternalError,
                              "Missing query pattern in update oplog entry.");
            }
            return handleOplogUpdate(opCtx, this, o, *o2);
        case 'd':
            return handleOplogDelete(this, o);
        case 'n':
            return Status::OK();
        case 'c':
            return Status(ErrorCodes::BadValue,
                          "Namespace admin.system.roles is not a valid target for commands");
        default:
            return Status(ErrorCodes::BadValue,
                          mongoutils::str::stream() << "Unrecognized \"op\" field value \"" << op
                                                    << '"');
    }
}

}  // namespace mongo
