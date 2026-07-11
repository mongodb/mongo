// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/modules.h"

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
