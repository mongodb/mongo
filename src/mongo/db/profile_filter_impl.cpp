/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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


#include "mongo/db/profile_filter_impl.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/compiler/dependency_analysis/match_expression_dependencies.h"
#include "mongo/db/server_options.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <memory>
#include <set>
#include <string>
#include <utility>

#include <absl/container/node_hash_map.h>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {

ProfileFilterImpl::ProfileFilterImpl(BSONObj expr,
                                     boost::intrusive_ptr<ExpressionContext> parserExpCtx)
    : _matcher(expr.getOwned(), parserExpCtx) {

    DepsTracker deps;
    dependency_analysis::addDependencies(_matcher.getMatchExpression(), &deps);
    uassert(4910201,
            "Profile filter is not allowed to depend on metadata",
            !deps.getNeedsAnyMetadata());

    // We only bother tracking top-level fields as dependencies.
    for (auto&& field : deps.fields) {
        _dependencies.emplace(FieldPath(std::move(field)).front());
    }
    _needWholeDocument = deps.needWholeDocument;

    // Remember a list of functions we'll call whenever we need to build BSON from CurOp.
    _makeBSON = OpDebug::appendStaged(
        parserExpCtx->getOperationContext(), _dependencies, _needWholeDocument);

    // The operation context is necessary for parsing, but should not be used for the rest of the
    // lifetime of the filter, since the filter exists for longer than a single operation.
    parserExpCtx->setOperationContext(nullptr);
}

bool ProfileFilterImpl::matches(OperationContext* opCtx,
                                const OpDebug& op,
                                const CurOp& curop) const {
    try {
        return exec::matcher::matches(&_matcher, _makeBSON({opCtx, op, curop}));
    } catch (const DBException& e) {
        LOGV2_DEBUG(4910202, 5, "Profile filter threw an exception", "exception"_attr = redact(e));
        return false;
    }
}

void ProfileFilterImpl::initializeDefaults(ServiceContext* service) {
    auto& dbProfileSettings = DatabaseProfileSettings::get(service);
    dbProfileSettings.setDefaultLevel(serverGlobalParams.defaultProfile);

    try {
        if (auto expr = serverGlobalParams.defaultProfileFilter) {
            // Create a temporary operation context that will only be valid for parsing, and will
            // be deleted after the try/catch block.
            const auto tempOpCtx = cc().makeOperationContext();
            dbProfileSettings.setDefaultFilter(std::make_shared<ProfileFilterImpl>(
                *expr, ExpressionContextBuilder{}.opCtx(tempOpCtx.get()).build()));
        }
    } catch (AssertionException& e) {
        // Add more context to the error
        uasserted(ErrorCodes::FailedToParse,
                  str::stream() << "Failed to parse option operationProfiling.filter: "
                                << e.reason());
    }
}
}  // namespace mongo
