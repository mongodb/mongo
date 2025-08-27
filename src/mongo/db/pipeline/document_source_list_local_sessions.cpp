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

#include "mongo/db/pipeline/document_source_list_local_sessions.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source_list_sessions_gen.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/session/logical_session_cache.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <algorithm>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

REGISTER_DOCUMENT_SOURCE(listLocalSessions,
                         DocumentSourceListLocalSessions::LiteParsed::parse,
                         DocumentSourceListLocalSessions::createFromBson,
                         AllowedWithApiStrict::kNeverInVersion1);
ALLOCATE_DOCUMENT_SOURCE_ID(listLocalSessions, DocumentSourceListLocalSessions::id)

boost::intrusive_ptr<DocumentSource> DocumentSourceListLocalSessions::createFromBson(
    BSONElement spec, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {

    uassert(
        ErrorCodes::InvalidNamespace,
        str::stream() << kStageName
                      << " must be run against the database with {aggregate: 1}, not a collection",
        pExpCtx->getNamespaceString().isCollectionlessAggregateNS());

    return new DocumentSourceListLocalSessions(pExpCtx, listSessionsParseSpec(kStageName, spec));
}

DocumentSourceListLocalSessions::DocumentSourceListLocalSessions(
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx, const ListSessionsSpec& spec)
    : DocumentSource(kStageName, pExpCtx), _spec(spec) {}

namespace {
ListSessionsUser getUserNameForLoggedInUser(const OperationContext* opCtx) {
    auto* client = opCtx->getClient();

    ListSessionsUser user;
    if (AuthorizationManager::get(client->getService())->isAuthEnabled()) {
        const auto& userName = AuthorizationSession::get(client)->getAuthenticatedUserName();
        uassert(ErrorCodes::Unauthorized, "There is no user authenticated", userName);
        user.setUser(userName->getUser());
        user.setDb(userName->getDB());
    } else {
        user.setUser("");
        user.setDb("");
    }
    return user;
}
}  // namespace

}  // namespace mongo

std::vector<mongo::SHA256Block> mongo::listSessionsUsersToDigests(
    const std::vector<ListSessionsUser>& users) {
    std::vector<SHA256Block> ret;
    ret.reserve(users.size());
    for (const auto& user : users) {
        ret.push_back(getLogicalSessionUserDigestFor(user.getUser(), user.getDb()));
    }
    return ret;
}

mongo::PrivilegeVector mongo::listSessionsRequiredPrivileges(
    const ListSessionsSpec& spec, const boost::optional<TenantId>& tenantId) {
    const auto needsPrivs = ([spec]() {
        if (spec.getAllUsers()) {
            return true;
        }
        // parseSpec should ensure users is non-empty.
        invariant(spec.getUsers());

        const auto& myName =
            getUserNameForLoggedInUser(Client::getCurrent()->getOperationContext());
        const auto& users = spec.getUsers().value();
        return !std::all_of(
            users.cbegin(), users.cend(), [myName](const auto& name) { return myName == name; });
    })();

    if (needsPrivs) {
        return {Privilege(ResourcePattern::forClusterResource(tenantId), ActionType::listSessions)};
    } else {
        return PrivilegeVector();
    }
}

mongo::ListSessionsSpec mongo::listSessionsParseSpec(StringData stageName,
                                                     const BSONElement& spec) {
    uassert(ErrorCodes::TypeMismatch,
            str::stream() << stageName << " options must be specified in an object, but found: "
                          << typeName(spec.type()),
            spec.type() == BSONType::object);

    IDLParserContext ctx(stageName);
    auto ret = ListSessionsSpec::parse(spec.Obj(), ctx);

    uassert(ErrorCodes::UnsupportedFormat,
            str::stream() << stageName
                          << " may not specify {allUsers:true} and {users:[...]} at the same time",
            !ret.getAllUsers() || !ret.getUsers() || ret.getUsers()->empty());

    // Verify that the correct state is set on the client.
    uassert(
        31106,
        str::stream() << "The " << DocumentSourceListLocalSessions::kStageName
                      << " stage is not allowed in this context :: missing an AuthorizationManager",
        AuthorizationManager::get(Client::getCurrent()->getService()));
    uassert(
        31111,
        str::stream() << "The " << DocumentSourceListLocalSessions::kStageName
                      << " stage is not allowed in this context :: missing a LogicalSessionCache",
        LogicalSessionCache::get(Client::getCurrent()->getOperationContext()));

    if (!ret.getAllUsers() && (!ret.getUsers() || ret.getUsers()->empty())) {
        // Implicit request for self
        const auto& userName =
            getUserNameForLoggedInUser(Client::getCurrent()->getOperationContext());
        ret.setUsers(std::vector<ListSessionsUser>({userName}));
    }

    return ret;
}
