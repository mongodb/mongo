/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/logical_session_id_helpers.h"
#include "mongo/db/pipeline/document_source_list_local_sessions.h"
#include "mongo/db/pipeline/document_sources_gen.h"

namespace mongo {

REGISTER_DOCUMENT_SOURCE(listLocalSessions,
                         DocumentSourceListLocalSessions::LiteParsed::parse,
                         DocumentSourceListLocalSessions::createFromBson);

const char* DocumentSourceListLocalSessions::kStageName = "$listLocalSessions";

DocumentSource::GetNextResult DocumentSourceListLocalSessions::getNext() {
    pExpCtx->checkForInterrupt();

    while (!_ids.empty()) {
        const auto& id = _ids.back();
        _ids.pop_back();

        const auto& record = _cache->peekCached(id);
        if (!record) {
            // It's possible for SessionRecords to have expired while we're walking
            continue;
        }
        return Document(record->toBSON());
    }

    return GetNextResult::makeEOF();
}

boost::intrusive_ptr<DocumentSource> DocumentSourceListLocalSessions::createFromBson(
    BSONElement spec, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {

    uassert(
        ErrorCodes::InvalidNamespace,
        str::stream() << kStageName
                      << " must be run against the database with {aggregate: 1}, not a collection",
        pExpCtx->ns.isCollectionlessAggregateNS());

    return new DocumentSourceListLocalSessions(pExpCtx, listSessionsParseSpec(kStageName, spec));
}

DocumentSourceListLocalSessions::DocumentSourceListLocalSessions(
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx, const ListSessionsSpec& spec)
    : DocumentSource(pExpCtx), _spec(spec) {
    const auto& opCtx = pExpCtx->opCtx;
    _cache = LogicalSessionCache::get(opCtx);
    if (_spec.getAllUsers()) {
        invariant(!_spec.getUsers() || _spec.getUsers()->empty());
        _ids = _cache->listIds();
    } else {
        _ids = _cache->listIds(listSessionsUsersToDigests(_spec.getUsers().get()));
    }
}

namespace {
ListSessionsUser getUserNameForLoggedInUser(const OperationContext* opCtx) {
    auto* client = opCtx->getClient();

    ListSessionsUser user;
    if (AuthorizationManager::get(client->getServiceContext())->isAuthEnabled()) {
        const auto& userName = AuthorizationSession::get(client)->getSingleUser()->getName();
        user.setUser(userName.getUser());
        user.setDb(userName.getDB());
    } else {
        user.setUser("");
        user.setDb("");
    }
    return user;
}

bool operator==(const ListSessionsUser& user1, const ListSessionsUser& user2) {
    return std::tie(user1.getUser(), user1.getDb()) == std::tie(user2.getUser(), user2.getDb());
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

mongo::PrivilegeVector mongo::listSessionsRequiredPrivileges(const ListSessionsSpec& spec) {
    const auto needsPrivs = ([spec]() {
        if (spec.getAllUsers()) {
            return true;
        }
        // parseSpec should ensure users is non-empty.
        invariant(spec.getUsers());

        const auto& myName =
            getUserNameForLoggedInUser(Client::getCurrent()->getOperationContext());
        const auto& users = spec.getUsers().get();
        return !std::all_of(
            users.cbegin(), users.cend(), [myName](const auto& name) { return myName == name; });
    })();

    if (needsPrivs) {
        return {Privilege(ResourcePattern::forClusterResource(), ActionType::listSessions)};
    } else {
        return PrivilegeVector();
    }
}

mongo::ListSessionsSpec mongo::listSessionsParseSpec(StringData stageName,
                                                     const BSONElement& spec) {
    uassert(ErrorCodes::TypeMismatch,
            str::stream() << stageName << " options must be specified in an object, but found: "
                          << typeName(spec.type()),
            spec.type() == BSONType::Object);

    IDLParserErrorContext ctx(stageName);
    auto ret = ListSessionsSpec::parse(ctx, spec.Obj());

    uassert(ErrorCodes::UnsupportedFormat,
            str::stream() << stageName
                          << " may not specify {allUsers:true} and {users:[...]} at the same time",
            !ret.getAllUsers() || !ret.getUsers() || ret.getUsers()->empty());

    if (!ret.getAllUsers() && (!ret.getUsers() || ret.getUsers()->empty())) {
        // Implicit request for self
        const auto& userName =
            getUserNameForLoggedInUser(Client::getCurrent()->getOperationContext());
        ret.setUsers(std::vector<ListSessionsUser>({userName}));
    }

    return ret;
}
