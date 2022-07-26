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

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/commands/profile_common.h"
#include "mongo/db/commands/profile_gen.h"
#include "mongo/db/curop.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/profile_filter_impl.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {

Status ProfileCmdBase::checkAuthForCommand(Client* client,
                                           const std::string& dbName,
                                           const BSONObj& cmdObj) const {
    AuthorizationSession* authzSession = AuthorizationSession::get(client);

    auto request = ProfileCmdRequest::parse(IDLParserContext("profile"), cmdObj);
    const auto profilingLevel = request.getCommandParameter();

    if (profilingLevel < 0 && !request.getSlowms() && !request.getSampleRate()) {
        // If the user just wants to view the current values of 'slowms' and 'sampleRate', they
        // only need read rights on system.profile, even if they can't change the profiling level.
        if (authzSession->isAuthorizedForActionsOnResource(
                ResourcePattern::forExactNamespace({dbName, "system.profile"}), ActionType::find)) {
            return Status::OK();
        }
    }

    return authzSession->isAuthorizedForActionsOnResource(ResourcePattern::forDatabaseName(dbName),
                                                          ActionType::enableProfiler)
        ? Status::OK()
        : Status(ErrorCodes::Unauthorized, "unauthorized");
}

bool ProfileCmdBase::run(OperationContext* opCtx,
                         const std::string& dbName,
                         const BSONObj& cmdObj,
                         BSONObjBuilder& result) {
    auto request = ProfileCmdRequest::parse(IDLParserContext("profile"), cmdObj);
    const auto profilingLevel = request.getCommandParameter();

    // Validate arguments before making changes.
    if (auto sampleRate = request.getSampleRate()) {
        uassert(ErrorCodes::BadValue,
                "'sampleRate' must be between 0.0 and 1.0 inclusive",
                *sampleRate >= 0.0 && *sampleRate <= 1.0);
    }

    // TODO SERVER-67459: For _applyProfilingLevel, takes the passed in "const DatabaseName& dbName"
    // directly.
    // Delegate to _applyProfilingLevel to set the profiling level appropriately whether
    // we are on mongoD or mongoS.
    auto oldSettings = _applyProfilingLevel(opCtx, {boost::none, dbName}, request);
    auto oldSlowMS = serverGlobalParams.slowMS;
    auto oldSampleRate = serverGlobalParams.sampleRate;

    result.append("was", oldSettings.level);
    result.append("slowms", oldSlowMS);
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
        serverGlobalParams.slowMS = *slowms;
    }
    if (auto sampleRate = request.getSampleRate()) {
        serverGlobalParams.sampleRate = *sampleRate;
    }

    // Log the change made to server's profiling settings, if the request asks to change anything.
    if (profilingLevel != -1 || request.getSlowms() || request.getSampleRate() ||
        request.getFilter()) {
        logv2::DynamicAttributes attrs;

        BSONObjBuilder oldState;
        BSONObjBuilder newState;

        oldState.append("level"_sd, oldSettings.level);
        oldState.append("slowms"_sd, oldSlowMS);
        oldState.append("sampleRate"_sd, oldSampleRate);
        if (oldSettings.filter) {
            oldState.append("filter"_sd, oldSettings.filter->serialize());
        }
        attrs.add("from", oldState.obj());

        // TODO SERVER-67459: For getDatabaseProfileSettings, takes the passed in "const
        // DatabaseName& dbName" directly.

        // newSettings.level may differ from profilingLevel: profilingLevel is part of the request,
        // and if the request specifies {profile: -1, ...} then we want to show the unchanged value
        // (0, 1, or 2).
        auto newSettings =
            CollectionCatalog::get(opCtx)->getDatabaseProfileSettings({boost::none, dbName});
        newState.append("level"_sd, newSettings.level);
        newState.append("slowms"_sd, serverGlobalParams.slowMS);
        newState.append("sampleRate"_sd, serverGlobalParams.sampleRate);
        if (newSettings.filter) {
            newState.append("filter"_sd, newSettings.filter->serialize());
        }
        attrs.add("to", newState.obj());

        LOGV2(48742, "Profiler settings changed", attrs);
    }

    return true;
}

ObjectOrUnset parseObjectOrUnset(const BSONElement& element) {
    if (element.type() == BSONType::Object) {
        return {{element.Obj()}};
    } else if (element.type() == BSONType::String && element.String() == "unset"_sd) {
        return {{}};
    } else {
        uasserted(ErrorCodes::BadValue, "Expected an object, or the string 'unset'.");
    }
}

void serializeObjectOrUnset(const ObjectOrUnset& obj,
                            StringData fieldName,
                            BSONObjBuilder* builder) {
    if (obj.obj) {
        builder->append(fieldName, *obj.obj);
    } else {
        builder->append(fieldName, "unset"_sd);
    }
}

}  // namespace mongo
