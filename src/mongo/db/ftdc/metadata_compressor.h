/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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
#include "mongo/db/ftdc/collector.h"

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
