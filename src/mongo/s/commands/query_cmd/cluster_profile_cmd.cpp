// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
        auto newSettings = oldSettings;

        if (auto filterOrUnset = request.getFilter()) {
            if (auto filter = filterOrUnset->obj) {
                newSettings.filter = std::make_shared<ProfileFilterImpl>(
                    *filter, ExpressionContextBuilder{}.opCtx(opCtx).build());
            } else {
                newSettings.filter = nullptr;
            }
        }

        if (auto slowOpInProgMS = request.getSlowinprogms()) {
            newSettings.slowOpInProgressThreshold = Milliseconds(*slowOpInProgMS);
        }

        if (oldSettings != newSettings) {
            dbProfileSettings.setDatabaseProfileSettings(dbName, newSettings);
        }

        return oldSettings;
    }
};

MONGO_REGISTER_COMMAND(ProfileCmd).forRouter();
MONGO_REGISTER_COMMAND(SetProfilingFilterGloballyCmd).forRouter();

}  // namespace
}  // namespace mongo
