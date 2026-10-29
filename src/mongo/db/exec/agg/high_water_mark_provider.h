// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/util/modules.h"

#include <boost/optional/optional.hpp>

namespace mongo {
namespace exec {
namespace agg {

/**
 * Implemented only by stages whose input is a resumable scan that can report how far it has
 * progressed, even when it currently produces no results. Stages that do not own such a scan do not
 * implement this interface. A stage that needs the position walks its source chain looking for the
 * first stage that does, using 'Stage::forEachSourceStage()'.
 */
class [[MONGO_MOD_OPEN]] HighWaterMarkProvider {
public:
    virtual ~HighWaterMarkProvider() = default;

    /**
     * Returns the cluster time of the current high water mark of the resumable scan underlying this
     * stage, or boost::none if there is no such scan or it has not reported a position yet. A high
     * water mark at cluster time 'T' guarantees that every event with a cluster time before 'T' has
     * already been produced by the scan.
     *
     * This method is not const because an implementation may have to advance the position it
     * reports as a side effect of being asked for it. Implementations must nevertheless never
     * discard or reorder results, and must never report a position which is ahead of the results
     * they have actually produced.
     */
    virtual boost::optional<Timestamp> getHighWaterMarkClusterTime() = 0;
};

}  // namespace agg
}  // namespace exec
}  // namespace mongo
