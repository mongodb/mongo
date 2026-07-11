// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic.h"
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
