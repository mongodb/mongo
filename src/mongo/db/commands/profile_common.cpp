// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/commands/profile_common.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands/profile_gen.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/profile_filter.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/server_options.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/attribute_storage.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string_view>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
using namespace std::literals::string_view_literals;

namespace {

// This assert is here to make sure new command options are considered in the 'isReadOnly' check. If
// this assert fails, please make sure you consider the authorization implications of your change.
MONGO_STATIC_ASSERT(ProfileCmdRequest::fieldMetadata.size() == 53);

bool isReadOnly(const ProfileCmdRequest& request) {
    return !request.getSlowms() && !request.getSlowinprogms() && !request.getSampleRate() &&
        !request.getFilter();
}
}  // namespace

Status ProfileCmdBase::checkAuthForOperation(OperationContext* opCtx,
                                             const DatabaseName& dbName,
                                             const BSONObj& cmdObj) const {
    AuthorizationSession* authzSession = AuthorizationSession::get(opCtx->getClient());

    const auto vts = auth::ValidatedTenancyScope::get(opCtx);
    auto sc = vts != boost::none
        ? SerializationContext::stateCommandRequest(vts->hasTenantId(), vts->isFromAtlasProxy())
        : SerializationContext::stateCommandRequest();

    auto request =
        ProfileCmdRequest::parse(cmdObj, IDLParserContext("profile", vts, dbName.tenantId(), sc));
    const auto profilingLevel = request.getCommandParameter();

    if (profilingLevel < 0 && isReadOnly(request)) {
        // If the user just wants to view the current profiling settings, they only need read rights
        // on system.profile, even if they can't change the profiling level.
        if (authzSession->isAuthorizedForActionsOnResource(
                ResourcePattern::forExactNamespace(
                    NamespaceStringUtil::deserialize(dbName, "system.profile")),
                ActionType::find)) {
            return Status::OK();
        }
    }

    return authzSession->isAuthorizedForActionsOnResource(ResourcePattern::forDatabaseName(dbName),
                                                          ActionType::enableProfiler)
        ? Status::OK()
        : Status(ErrorCodes::Unauthorized, "unauthorized");
}

bool ProfileCmdBase::run(OperationContext* opCtx,
                         const DatabaseName& dbName,
                         const BSONObj& cmdObj,
                         BSONObjBuilder& result) {
    const auto vts = auth::ValidatedTenancyScope::get(opCtx);
    const auto sc = vts != boost::none
        ? SerializationContext::stateCommandRequest(vts->hasTenantId(), vts->isFromAtlasProxy())
        : SerializationContext::stateCommandRequest();
    auto request =
        ProfileCmdRequest::parse(cmdObj, IDLParserContext("profile", vts, dbName.tenantId(), sc));
    const auto profilingLevel = request.getCommandParameter();

    // Validate arguments before making changes.
    if (auto sampleRate = request.getSampleRate()) {
        uassert(ErrorCodes::BadValue,
                "'sampleRate' must be between 0.0 and 1.0 inclusive",
                *sampleRate >= 0.0 && *sampleRate <= 1.0);
    }

    // Delegate to _applyProfilingLevel to set the profiling level appropriately whether
    // we are on mongoD or mongoS.
    auto oldSettings = _applyProfilingLevel(opCtx, dbName, request);
    auto oldSlowMS = serverGlobalParams.slowMS.load();
    auto oldSampleRate = serverGlobalParams.sampleRate.load();

    result.append("was", oldSettings.level);
    result.append("slowms", oldSlowMS);
    result.append("slowinprogms", oldSettings.slowOpInProgressThreshold.count());
    result.append("sampleRate", oldSampleRate);
    if (oldSettings.filter) {
        result.append("filter", oldSettings.filter->serialize());
    }
    if (oldSettings.filter || request.getFilter()) {
        result.append("note",
                      "When a filter expression is set, slowms and sampleRate are not used for "
                      "profiling and slow-query log lines.");
    }

    if (auto slowms = request.getSlowms()) {
        serverGlobalParams.slowMS.store(*slowms);
    }
    if (auto sampleRate = request.getSampleRate()) {
        serverGlobalParams.sampleRate.store(*sampleRate);
    }

    // Log the change made to server's profiling settings, if the request asks to change anything.
    if (profilingLevel != -1 || request.getSlowms() || request.getSlowinprogms() ||
        request.getSampleRate() || request.getFilter()) {
        logv2::DynamicAttributes attrs;

        BSONObjBuilder oldState;
        BSONObjBuilder newState;

        oldState.append("level"sv, oldSettings.level);
        oldState.append("slowms"sv, oldSlowMS);
        oldState.append("slowinprogms"sv, oldSettings.slowOpInProgressThreshold.count());
        oldState.append("sampleRate"sv, oldSampleRate);
        if (oldSettings.filter) {
            oldState.append("filter"sv, oldSettings.filter->serialize());
        }
        attrs.add("from", oldState.obj());

        // newSettings.level may differ from profilingLevel: profilingLevel is part of the request,
        // and if the request specifies {profile: -1, ...} then we want to show the unchanged value
        // (0, 1, or 2).
        auto& dbProfileSettings = DatabaseProfileSettings::get(opCtx->getServiceContext());
        auto newSettings = dbProfileSettings.getDatabaseProfileSettings(dbName);
        newState.append("level"sv, newSettings.level);
        newState.append("slowms"sv, serverGlobalParams.slowMS.load());
        newState.append("slowinprogms"sv, newSettings.slowOpInProgressThreshold.count());
        newState.append("sampleRate"sv, serverGlobalParams.sampleRate.load());
        if (newSettings.filter) {
            newState.append("filter"sv, newSettings.filter->serialize());
        }
        attrs.add("to", newState.obj());
        attrs.add("db", dbName);

        LOGV2(48742, "Profiler settings changed", attrs);
    }

    return true;
}

ObjectOrUnset parseObjectOrUnset(const BSONElement& element) {
    if (element.type() == BSONType::object) {
        return {{element.Obj()}};
    } else if (element.type() == BSONType::string && element.String() == "unset"sv) {
        return {{}};
    } else {
        uasserted(ErrorCodes::BadValue, "Expected an object, or the string 'unset'.");
    }
}

void serializeObjectOrUnset(const ObjectOrUnset& obj,
                            std::string_view fieldName,
                            BSONObjBuilder* builder) {
    if (obj.obj) {
        builder->append(fieldName, *obj.obj);
    } else {
        builder->append(fieldName, "unset"sv);
    }
}

}  // namespace mongo
