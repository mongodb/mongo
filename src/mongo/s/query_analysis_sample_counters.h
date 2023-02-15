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

#include "mongo/db/namespace_string.h"
#include "mongo/util/uuid.h"

#include <boost/optional.hpp>
#include <map>
#include <vector>

namespace mongo {
namespace analyze_shard_key {

class SampleCounters {
public:
    static const std::string kDescriptionFieldName;
    static const std::string kDescriptionFieldValue;
    static const std::string kNamespaceStringFieldName;
    static const std::string kCollUuidFieldName;
    static const std::string kSampledReadsCountFieldName;
    static const std::string kSampledWritesCountFieldName;
    static const std::string kSampledReadsBytesFieldName;
    static const std::string kSampledWritesBytesFieldName;
    static const std::string kSampleRateFieldName;

    SampleCounters(const NamespaceString& nss, const UUID& collUuid)
        : _nss(nss),
          _collUuid(collUuid),
          _sampledReadsCount(0LL),
          _sampledReadsBytes(0LL),
          _sampledWritesCount(0LL),
          _sampledWritesBytes(0LL){};

    NamespaceString getNss() const {
        return _nss;
    }

    void setNss(const NamespaceString& nss) {
        _nss = nss;
    }

    UUID getCollUUID() const {
        return _collUuid;
    }

    size_t getSampledReadsCount() const {
        return _sampledReadsCount;
    }

    size_t getSampledReadsBytes() const {
        return _sampledReadsBytes;
    }

    size_t getSampledWritesCount() const {
        return _sampledWritesCount;
    }

    size_t getSampledWritesBytes() const {
        return _sampledWritesBytes;
    }

    /**
     * Increments the read counter and adds <size> to the read bytes counter.
     */
    void incrementReads(boost::optional<long long> size) {
        ++_sampledReadsCount;
        if (size) {
            _sampledReadsBytes += *size;
        }
    }

    /**
     * Increments the write counter and adds <size> to the write bytes counter.
     */
    void incrementWrites(boost::optional<long long> size) {
        ++_sampledWritesCount;
        if (size) {
            _sampledWritesBytes += *size;
        }
    }

    BSONObj reportCurrentOp() const;

private:
    NamespaceString _nss;
    const UUID _collUuid;
    long long _sampledReadsCount;
    long long _sampledReadsBytes;
    long long _sampledWritesCount;
    long long _sampledWritesBytes;
};

}  // namespace analyze_shard_key
}  // namespace mongo
