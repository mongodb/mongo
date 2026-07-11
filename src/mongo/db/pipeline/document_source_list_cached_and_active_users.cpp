// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

REGISTER_TEST_LITE_PARSED_DOCUMENT_SOURCE(
    listCachedAndActiveUsers, DocumentSourceListCachedAndActiveUsers::LiteParsed::parse);

REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(listCachedAndActiveUsers,
                                                   DocumentSourceListCachedAndActiveUsers,
                                                   ListCachedAndActiveUsersStageParams);

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
