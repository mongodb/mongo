/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#pragma once


#include "mongo/bson/bsonobj.h"
#include "mongo/db/query/record_id_bound.h"

#include <boost/optional.hpp>

namespace mongo {

class RecordIdRange {
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
