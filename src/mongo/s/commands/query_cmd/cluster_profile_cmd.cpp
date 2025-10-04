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

#include "mongo/base/error_codes.h"
#include "mongo/db/commands/profile_common.h"
#include "mongo/db/commands/profile_gen.h"
#include "mongo/db/commands/set_profiling_filter_globally_cmd.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/profile_filter_impl.h"
#include "mongo/db/profile_settings.h"
#include "mongo/util/assert_util.h"

#include <memory>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

class ProfileCmd : public ProfileCmdBase {
public:
    ProfileCmd() = default;

    // Although mongoS does not have a system.profile collection, the profile command can change the
    // per-database profile filter, which applies to slow-query log lines just like on mongoD.
    bool adminOnly() const final {
        return false;
    }

protected:
    ProfileSettings _applyProfilingLevel(OperationContext* opCtx,
                                         const DatabaseName& dbName,
                                         const ProfileCmdRequest& request) const final {
        const auto profilingLevel = request.getCommandParameter();

        // The only valid profiling level for mongoS is 0, because mongoS has no system.profile
        // collection in which to record the profiling data (because mongoS has no collections
        // at all).
        uassert(ErrorCodes::BadValue,
                "Profiling is not permitted on mongoS: the 'profile' field should be 0 to change "
                "'slowms', 'sampleRate', or 'filter' settings for logging, or -1 to view current "
                "values",
                profilingLevel == -1 || profilingLevel == 0);

        auto& dbProfileSettings = DatabaseProfileSettings::get(opCtx->getServiceContext());
        auto oldSettings = dbProfileSettings.getDatabaseProfileSettings(dbName);

        if (auto filterOrUnset = request.getFilter()) {
            auto newSettings = oldSettings;
            if (auto filter = filterOrUnset->obj) {
                newSettings.filter = std::make_shared<ProfileFilterImpl>(
                    *filter, ExpressionContextBuilder{}.opCtx(opCtx).build());
            } else {
                newSettings.filter = nullptr;
            }
            dbProfileSettings.setDatabaseProfileSettings(dbName, newSettings);
        }

        return oldSettings;
    }
};

MONGO_REGISTER_COMMAND(ProfileCmd).forRouter();
MONGO_REGISTER_COMMAND(SetProfilingFilterGloballyCmd).forRouter();

}  // namespace
}  // namespace mongo
