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

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/tenant_id.h"

#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class V2UserDocumentParser {
    V2UserDocumentParser(const V2UserDocumentParser&) = delete;
    V2UserDocumentParser& operator=(const V2UserDocumentParser&) = delete;

public:
    V2UserDocumentParser() {}

    /**
     * Apply a tenant identifier to every tenant aware object during parsing.
     */
    void setTenantId(boost::optional<TenantId> tenant) {
        _tenant = std::move(tenant);
    }

    Status checkValidUserDocument(const BSONObj& doc) const;
    Status initializeUserFromUserDocument(const BSONObj& privDoc, User* user) const;

private:
    Status initializeUserIndirectRolesFromUserDocument(const BSONObj& doc, User* user) const;
    Status initializeUserPrivilegesFromUserDocument(const BSONObj& doc, User* user) const;

public:
    // public for unit testing only.
    Status initializeUserCredentialsFromUserDocument(User* user, const BSONObj& privDoc) const;
    Status initializeUserRolesFromUserDocument(const BSONObj& doc, User* user) const;
    Status initializeAuthenticationRestrictionsFromUserDocument(const BSONObj& doc,
                                                                User* user) const;

private:
    boost::optional<TenantId> _tenant;
};

}  // namespace mongo
