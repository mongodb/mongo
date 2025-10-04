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

#include "mongo/db/pipeline/document_source_list_cached_and_active_users.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source_queue.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"

#include <iterator>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {
Document makeDocumentFromCachedUserInfo(const AuthorizationRouter::CachedUserInfo& user) {
    return Document(BSON("username" << user.userName.getUser() << "db" << user.userName.getDB()
                                    << "active" << user.active));
}
}  // namespace

REGISTER_TEST_DOCUMENT_SOURCE(listCachedAndActiveUsers,
                              DocumentSourceListCachedAndActiveUsers::LiteParsed::parse,
                              DocumentSourceListCachedAndActiveUsers::createFromBson);

boost::intrusive_ptr<DocumentSource> DocumentSourceListCachedAndActiveUsers::createFromBson(
    BSONElement spec, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(ErrorCodes::BadValue,
            str::stream() << kStageName << " must be run as { " << kStageName << ": {}}",
            spec.isABSONObj() && spec.Obj().isEmpty());
    auto users =
        AuthorizationManager::get(pExpCtx->getOperationContext()->getService())->getUserCacheInfo();
    std::deque<DocumentSource::GetNextResult> queue;
    std::transform(
        users.begin(), users.end(), std::back_inserter(queue), makeDocumentFromCachedUserInfo);

    return make_intrusive<DocumentSourceQueue>(std::move(queue), pExpCtx, kStageName);
}

}  // namespace mongo
