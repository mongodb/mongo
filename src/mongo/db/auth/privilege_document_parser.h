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
         * Returns the name of the user in the given privilege document.
         */
        virtual std::string extractUserNameFromPrivilegeDocument(const BSONObj& doc) const = 0;

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

    };

    class V1PrivilegeDocumentParser : public PrivilegeDocumentParser {
        MONGO_DISALLOW_COPYING(V1PrivilegeDocumentParser);
    public:

        V1PrivilegeDocumentParser() {}
        virtual ~V1PrivilegeDocumentParser() {}

        virtual std::string extractUserNameFromPrivilegeDocument(const BSONObj& doc) const;

        virtual Status initializeUserCredentialsFromPrivilegeDocument(User* user,
                                                                      const BSONObj& privDoc) const;

        virtual Status initializeUserRolesFromPrivilegeDocument(
                        User* user, const BSONObj& privDoc, const StringData& dbname) const;
    };

    class V2PrivilegeDocumentParser : public PrivilegeDocumentParser {
        MONGO_DISALLOW_COPYING(V2PrivilegeDocumentParser);
    public:

        V2PrivilegeDocumentParser() {}
        virtual ~V2PrivilegeDocumentParser() {}

        Status checkValidPrivilegeDocument(const BSONObj& doc) const;

        /**
         * Returns Status::OK() iff the given BSONObj describes a valid element from a roles array.
         */
        Status checkValidRoleObject(const BSONObj& roleObject) const;

        virtual std::string extractUserNameFromPrivilegeDocument(const BSONObj& doc) const;

        virtual Status initializeUserCredentialsFromPrivilegeDocument(User* user,
                                                                      const BSONObj& privDoc) const;

        virtual Status initializeUserRolesFromPrivilegeDocument(
                        User* user, const BSONObj& privDoc, const StringData& unused) const;
    };

} // namespace mongo
