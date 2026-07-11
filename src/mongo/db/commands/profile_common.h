// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/service_context.h"
#include "mongo/util/modules.h"

#include <string>
#include <string_view>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class ProfileCmdRequest;

/**
 * An abstract base class which implements all functionality common to the mongoD and mongoS
 * 'profile' command, and defines a number of virtual functions through which it delegates any
 * op-specific work to its derived classes.
 */
class ProfileCmdBase : public BasicCommand {
public:
    ProfileCmdBase() : BasicCommand("profile") {}
    ~ProfileCmdBase() override {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kAlways;
    }

    std::string help() const final {
        return "controls the behaviour of the performance profiler, the fraction of eligible "
               "operations which are sampled for logging/profiling, and the threshold duration at "
               "which ops become eligible. See "
               "http://docs.mongodb.org/manual/reference/command/profile";
    }

    bool supportsWriteConcern(const BSONObj& cmd) const final {
        return false;
    }

    Status checkAuthForOperation(OperationContext*,
                                 const DatabaseName&,
                                 const BSONObj&) const final;

    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) final;

protected:
    // Applies the given profiling level and filter, or throws if the profiling level could not be
    // set. On success, returns a struct indicating the previous profiling level and filter.
    virtual ProfileSettings _applyProfilingLevel(OperationContext* opCtx,
                                                 const DatabaseName& dbName,
                                                 const ProfileCmdRequest& request) const = 0;
};

struct ObjectOrUnset {
    boost::optional<BSONObj> obj;
};
ObjectOrUnset parseObjectOrUnset(const BSONElement& element);
void serializeObjectOrUnset(const ObjectOrUnset& obj,
                            std::string_view fieldName,
                            BSONObjBuilder* builder);

}  // namespace mongo
