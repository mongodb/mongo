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
*/

#pragma once

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/jsobj.h"

namespace mongo {

    /**
     * Interface for class used to initialize User objects from their privilege documents.
     */
    class PrivilegeDocumentParser {
        MONGO_DISALLOW_COPYING(PrivilegeDocumentParser);
    public:
        PrivilegeDocumentParser() {};
        virtual ~PrivilegeDocumentParser() {};

        /**
         *  Returns an ActionSet of all actions that can be be granted to users.  This does not
         *  include internal-only actions.
         */
        virtual ActionSet getAllUserActions() const;

        /**
         * Returns Status::OK() if the given privilege document is valid to be inserted for a user
         * in the given database, returns a non-OK status otherwise.
         */
        virtual Status checkValidPrivilegeDocument(const StringData& dbname,
                                                   const BSONObj& doc) const = 0;

        /**
         * Parses privDoc and fully initializes the user object (credentials, roles, and privileges)
         * with the information extracted from the privilege document.
         */
        virtual Status initializeUserFromPrivilegeDocument(User* user,
                                                           const BSONObj& privDoc) const;

        /**
         * Parses privDoc and initializes the user's "credentials" field with the credential
         * information extracted from the privilege document.
         */
        virtual Status initializeUserCredentialsFromPrivilegeDocument(
                User* user, const BSONObj& privDoc) const = 0;

        /**
         * Parses privDoc and initializes the user's "roles" field with the role list extracted
         * from the privilege document.
         */
        virtual Status initializeUserRolesFromPrivilegeDocument(
                User* user, const BSONObj& privDoc, const StringData& dbname) const = 0;

        /**
         * Modifies the given User object by inspecting its roles and giving it the relevant
         * privileges from those roles.
         */
        virtual void initializeUserPrivilegesFromRoles(User* user) const = 0;

    };

    class V1PrivilegeDocumentParser : public PrivilegeDocumentParser {
        MONGO_DISALLOW_COPYING(V1PrivilegeDocumentParser);
    public:

        V1PrivilegeDocumentParser() {}
        virtual ~V1PrivilegeDocumentParser() {}

        virtual Status checkValidPrivilegeDocument(const StringData& dbname,
                                                   const BSONObj& doc) const;

        virtual Status initializeUserCredentialsFromPrivilegeDocument(User* user,
                                                                      const BSONObj& privDoc) const;

        virtual Status initializeUserRolesFromPrivilegeDocument(
                        User* user, const BSONObj& privDoc, const StringData& dbname) const;

        virtual void initializeUserPrivilegesFromRoles(User* user) const;
    };

    class V2PrivilegeDocumentParser : public PrivilegeDocumentParser {
        MONGO_DISALLOW_COPYING(V2PrivilegeDocumentParser);
    public:

        V2PrivilegeDocumentParser() {}
        virtual ~V2PrivilegeDocumentParser() {}

        virtual Status checkValidPrivilegeDocument(const StringData& dbname,
                                                   const BSONObj& doc) const;

        virtual Status initializeUserCredentialsFromPrivilegeDocument(User* user,
                                                                      const BSONObj& privDoc) const;

        virtual Status initializeUserRolesFromPrivilegeDocument(
                        User* user, const BSONObj& privDoc, const StringData& dbname) const;

        virtual void initializeUserPrivilegesFromRoles(User* user) const;
    };

} // namespace mongo
