// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/read_write_concern_defaults_cache_lookup_mock.h"

#include "mongo/util/assert_util.h"

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

ReadWriteConcernDefaults::FetchDefaultsFn ReadWriteConcernDefaultsLookupMock::getFetchDefaultsFn() {
    return [this](OperationContext* opCtx) {
        return lookup(opCtx);
    };
}

boost::optional<RWConcernDefault> ReadWriteConcernDefaultsLookupMock::lookup(
    OperationContext* opCtx) {
    uassert(_status->code(), _status->reason(), !_status);
    return _value;
}

void ReadWriteConcernDefaultsLookupMock::setLookupCallReturnValue(RWConcernDefault&& rwc) {
    _value.emplace(rwc);
}

void ReadWriteConcernDefaultsLookupMock::setLookupCallFailure(boost::optional<Status> status) {
    _status = status;
}

}  // namespace mongo
