// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once


#include "mongo/bson/bsonobj.h"
#include "mongo/db/query/record_id_bound.h"
#include "mongo/db/record_id.h"
#include "mongo/util/modules.h"

#include <boost/optional.hpp>

namespace mongo {

class [[MONGO_MOD_PUBLIC]] RecordIdRange {
public:
    /**
     * If the provided value @p newMin is greater than the existing min,
     * update the lower bound to equal @p newMin
     *
     * @return true if range was adjusted
     */
    void maybeNarrowMin(const BSONObj& newMin, bool inclusive);
    void maybeNarrowMin(const RecordIdBound& newMin, bool inclusive);

    /**
     * If the provided value @p newMax is less than the existing max,
     * update the upper bound to equal @p newMax
     *
     * @return true if range was adjusted
     */
    void maybeNarrowMax(const BSONObj& newMax, bool inclusive);
    void maybeNarrowMax(const RecordIdBound& newMax, bool inclusive);

    /**
     * Update this range to the intersection of this range
     * and @p other. This may update both, one of, or neither of
     * min and max.
     *
     * Results in a range which is either unchanged, or made
     * narrower (possibly becoming an empty range).
     */
    void intersectRange(const RecordIdRange& other);
    /**
     * Overload of intersectRange taking the components of a RecordIdRange,
     * for convenience when the other range is not handled as a RecordIdRange.
     */
    void intersectRange(const boost::optional<RecordIdBound>& min,
                        const boost::optional<RecordIdBound>& max,
                        bool minInclusive = true,
                        bool maxInclusive = true);

    bool isEmpty() const;

    /**
     * Compares a RecordId against this range.
     * Returns -1 if rid is before the start of this range (only possible when min is bounded;
     *           an absent min is treated as -∞ so rid is never before the start).
     * Returns  0 if rid is within this range.
     * Returns +1 if rid is past the end of this range (only possible when max is bounded;
     *           an absent max is treated as +∞ so rid is never past the end).
     */
    int compare(const RecordId& rid) const;

    const auto& getMin() const {
        return _min;
    }

    const auto& getMax() const {
        return _max;
    }

    bool isMinInclusive() const {
        return _minInclusive;
    }

    bool isMaxInclusive() const {
        return _maxInclusive;
    }


private:
    // If present, this parameter sets the start point of a forward scan or the end point of a
    // reverse scan.
    boost::optional<RecordIdBound> _min;

    // If present, this parameter sets the start point of a reverse scan or the end point of a
    // forward scan.
    boost::optional<RecordIdBound> _max;

    // TODO: investigate folding this into RecordIdBound; many other usages pair RecordIdBound
    //       with ScanBoundInclusion to convey this information
    // If min is present, this indicates whether the range is inclusive or exclusive of the
    // set min value
    bool _minInclusive = true;
    // If max is present, this indicates whether the range is inclusive or exclusive of the
    // set max value
    bool _maxInclusive = true;
};

}  // namespace mongo
