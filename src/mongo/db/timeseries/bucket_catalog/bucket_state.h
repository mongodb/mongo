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

#include <cstdint>
#include <string>

namespace mongo::timeseries::bucket_catalog {

enum class BucketStateFlag : std::uint8_t {
    // Bucket has a prepared batch outstanding.
    kPrepared = 0b00000001,
    // In-memory representation of the bucket may be out of sync with on-disk data. Bucket
    // should not be inserted into.
    kCleared = 0b00000010,
    // Bucket is effectively closed, but has an outstanding compression operation pending, so it
    // is also not eligible for reopening.
    kPendingCompression = 0b00000100,
    // Bucket is effectively closed, but has an outstanding direct write pending, so it is also
    // not eligible for reopening.
    kPendingDirectWrite = 0b00001000,
    // Bucket state is stored in the catalog for synchronization purposes only, but the actual
    // bucket isn't stored in the catalog, nor is it archived.
    kUntracked = 0b00010000,
};

class BucketState {
public:
    BucketState& setFlag(BucketStateFlag);
    BucketState& unsetFlag(BucketStateFlag);
    BucketState& reset();

    bool isSet(BucketStateFlag) const;
    bool isPrepared() const;
    bool conflictsWithReopening() const;
    bool conflictsWithInsertion() const;

    bool operator==(const BucketState&) const;
    std::string toString() const;

private:
    std::underlying_type<BucketStateFlag>::type _state = 0;
};

}  // namespace mongo::timeseries::bucket_catalog
