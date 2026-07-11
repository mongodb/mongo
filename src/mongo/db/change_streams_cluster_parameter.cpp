// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/change_streams_cluster_parameter.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/change_streams_cluster_parameter_gen.h"
#include "mongo/db/server_options.h"

#include <boost/optional/optional.hpp>

namespace mongo {

Status validateChangeStreamsClusterParameter(
    const ChangeStreamsClusterParameterStorage& clusterParameter,
    const boost::optional<TenantId>&) {
    if (clusterParameter.getExpireAfterSeconds() <= 0) {
        return Status(ErrorCodes::BadValue,
                      "Expected a positive integer for 'expireAfterSeconds' field");
    }

    return Status::OK();
}

}  // namespace mongo
