// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/read_write_concern_defaults_gen.h"
#include "mongo/util/modules.h"

#include <absl/container/node_hash_map.h>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * A class which handles looking up RWConcernDefault values from an in-memory location.
 */
class [[MONGO_MOD_PUBLIC]] ReadWriteConcernDefaultsLookupMock {
public:
    ReadWriteConcernDefaults::FetchDefaultsFn getFetchDefaultsFn();

    boost::optional<RWConcernDefault> lookup(OperationContext* opCtx);

    /**
     * Behind-the-scenes way to update the stored in-memory value that lookup() returns.
     */
    void setLookupCallReturnValue(RWConcernDefault&& value);

    /**
     * Set a status that lookup() should throw (or boost::none to not throw an exception).
     */
    void setLookupCallFailure(boost::optional<Status> status);

private:
    boost::optional<RWConcernDefault> _value;
    boost::optional<Status> _status;
};

}  // namespace mongo
