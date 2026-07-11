// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/repl/speculative_majority_read_info.h"

#include "mongo/db/operation_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

#include <algorithm>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace repl {

/**
 * An instance of SpeculativeReadInfo is stored as a decoration on the OperationContext, so that
 * each operation can optionally utilize this structure to perform speculative reads.
 */
const OperationContext::Decoration<SpeculativeMajorityReadInfo> handle =
    OperationContext::declareDecoration<SpeculativeMajorityReadInfo>();

SpeculativeMajorityReadInfo& SpeculativeMajorityReadInfo::get(OperationContext* opCtx) {
    return handle(opCtx);
}

void SpeculativeMajorityReadInfo::setIsSpeculativeRead() {
    _isSpeculativeRead = true;
}

bool SpeculativeMajorityReadInfo::isSpeculativeRead() const {
    return _isSpeculativeRead;
}

void SpeculativeMajorityReadInfo::setSpeculativeReadTimestampForward(const Timestamp& ts) {
    invariant(_isSpeculativeRead);
    // Set the timestamp initially if needed. Update it only if the given timestamp is greater.
    _speculativeReadTimestamp =
        _speculativeReadTimestamp ? std::max(*_speculativeReadTimestamp, ts) : ts;
}

boost::optional<Timestamp> SpeculativeMajorityReadInfo::getSpeculativeReadTimestamp() {
    invariant(_isSpeculativeRead);
    return _speculativeReadTimestamp;
}

}  // namespace repl
}  // namespace mongo
