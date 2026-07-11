// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/ftdc/collector.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <cstdint>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * FTDCMetadataCompressor is responsible for taking in a BSONObj sample containing collected
 * responses from commands that return system metadata (e.g. getParameter, getClusterParameter),
 * and returning a "delta" document containing the fields that changed since the previous sample
 * document (i.e. the "reference" document). It also keeps track of the number of times the
 * reference has been changed.
 *
 * For the FTDCMetadataCompressor to return only the changed fields between the sample and
 * reference, both the sample and reference must have the same sequence of field names at
 * the top-level and at the next level of subobjects. If the sequence of fields differ, then it will
 * reset the delta counter to 0 and set the current sample as the new reference; in this case,
 * instead of returning just the "delta" document, it returns the new reference entirely.
 */
class FTDCMetadataCompressor {
public:
    boost::optional<BSONObj> addSample(const BSONObj& sample);

    std::uint32_t getDeltaCount() const {
        return _deltaCount;
    }

    /**
     * Reset the state of the compressor.
     *
     * Callers can use this to reset the compressor to a clean state instead of recreating it.
     */
    void reset();

private:
    void _reset(const BSONObj& newReference);

private:
    // Reference schema document
    BSONObj _referenceDoc;

    // Number of deltas recorded
    std::uint32_t _deltaCount{0};
};
}  // namespace mongo
