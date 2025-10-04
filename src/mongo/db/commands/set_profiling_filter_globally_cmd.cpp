/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/commands/set_profiling_filter_globally_cmd.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands/profile_common.h"
#include "mongo/db/commands/profile_gen.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/profile_filter.h"
#include "mongo/db/profile_filter_impl.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <memory>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {

Status SetProfilingFilterGloballyCmd::checkAuthForOperation(OperationContext* opCtx,
                                                            const DatabaseName& dbName,
                                                            const BSONObj& cmdObj) const {
    AuthorizationSession* authSession = AuthorizationSession::get(opCtx->getClient());
    return authSession->isAuthorizedForActionsOnResource(
               ResourcePattern::forAnyNormalResource(dbName.tenantId()), ActionType::enableProfiler)
        ? Status::OK()
        : Status(ErrorCodes::Unauthorized, "unauthorized");
}

bool SetProfilingFilterGloballyCmd::run(OperationContext* opCtx,
                                        const DatabaseName& dbName,
                                        const BSONObj& cmdObj,
                                        BSONObjBuilder& result) {
    uassert(7283301,
            str::stream() << getName() << " command requires query knob to be enabled",
            internalQueryGlobalProfilingFilter.load());

    auto request = SetProfilingFilterGloballyCmdRequest::parse(cmdObj, IDLParserContext(getName()));

    auto& dbProfileSettings = DatabaseProfileSettings::get(opCtx->getServiceContext());

    // Save off the old global default setting so that we can log it and return in the result.
    auto oldDefault = dbProfileSettings.getDefaultFilter();
    auto newDefault = [&request, opCtx] {
        const auto& filterOrUnset = request.getFilter();
        if (auto filter = filterOrUnset.obj) {
            return std::make_shared<ProfileFilterImpl>(
                *filter, ExpressionContextBuilder{}.opCtx(opCtx).build());
        }
        return std::shared_ptr<ProfileFilterImpl>(nullptr);
    }();

    // Update the global default.
    // There is a minor race condition where queries on some databases see the new global default
    // while queries on other databases see old database-specific settings. This is a temporary
    // state and shouldn't impact much in practice. We also don't have to worry about races with
    // database creation, since the global default gets picked up dynamically by queries instead of
    // being explicitly stored for new databases.
    dbProfileSettings.setAllDatabaseProfileFiltersAndDefault(newDefault);

    // Capture the old setting in the result object.
    if (oldDefault) {
        result.append("was", oldDefault->serialize());
    } else {
        result.append("was", "none");
    }

    // Log the change made to server's global profiling settings.
    LOGV2(72832,
          "Profiler settings changed globally",
          "from"_attr = oldDefault ? BSON("filter" << redact(oldDefault->serialize()))
                                   : BSON("filter" << "none"),
          "to"_attr = newDefault ? BSON("filter" << redact(newDefault->serialize()))
                                 : BSON("filter" << "none"));
    return true;
}

MONGO_REGISTER_COMMAND(SetProfilingFilterGloballyCmd).forShard();

}  // namespace mongo
