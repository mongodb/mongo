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

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source_list_cached_and_active_users.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/logical_session_id_helpers.h"

namespace mongo {

REGISTER_TEST_DOCUMENT_SOURCE(listCachedAndActiveUsers,
                              DocumentSourceListCachedAndActiveUsers::LiteParsed::parse,
                              DocumentSourceListCachedAndActiveUsers::createFromBson);

const char* DocumentSourceListCachedAndActiveUsers::kStageName = "$listCachedAndActiveUsers";

DocumentSource::GetNextResult DocumentSourceListCachedAndActiveUsers::getNext() {
    pExpCtx->checkForInterrupt();

    if (!_users.empty()) {
        const auto info = std::move(_users.back());
        _users.pop_back();
        return Document(BSON("username" << info.userName.getUser() << "db" << info.userName.getDB()
                                        << "active"
                                        << info.active));
    }

    return GetNextResult::makeEOF();
}

boost::intrusive_ptr<DocumentSource> DocumentSourceListCachedAndActiveUsers::createFromBson(
    BSONElement spec, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {

    uassert(
        ErrorCodes::InvalidNamespace,
        str::stream() << kStageName
                      << " must be run against the database with {aggregate: 1}, not a collection",
        pExpCtx->ns.isCollectionlessAggregateNS());

    uassert(ErrorCodes::BadValue,
            str::stream() << kStageName << " must be run as { " << kStageName << ": {}}",
            spec.isABSONObj() && spec.Obj().isEmpty());

    return new DocumentSourceListCachedAndActiveUsers(pExpCtx);
}

DocumentSourceListCachedAndActiveUsers::DocumentSourceListCachedAndActiveUsers(
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx)
    : DocumentSource(pExpCtx), _users() {
    auto authMgr = AuthorizationManager::get(pExpCtx->opCtx->getServiceContext());
    _users = authMgr->getUserCacheInfo();
}

}  // namespace mongo
