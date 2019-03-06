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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/speculative_majority_read_info.h"
#include "mongo/util/decorable.h"
#include "mongo/util/log.h"

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
