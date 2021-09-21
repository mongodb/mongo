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

#pragma once

#include <cstdint>

namespace mongo {
/**
 * Controls the maximum BSON depth tolerated by the server.
 */
struct BSONDepth {
    // The default BSON depth nesting limit.
    static constexpr std::int32_t kDefaultMaxAllowableDepth = 200;

    // The number of extra levels of nesting above the storage depth limit that the server will
    // tolerate.
    static constexpr std::uint32_t kExtraSystemDepthLevels = 20;

    // The minimum allowable value for the BSON depth parameter. Choose a value such that the max
    // depth for user storage will be at least 1.
    static constexpr std::int32_t kBSONDepthParameterFloor = kExtraSystemDepthLevels + 1;

    // The maximum allowable value for the BSON depth parameter.
    static constexpr std::int32_t kBSONDepthParameterCeiling = 250;

    // The depth of BSON accepted by the server. Configurable via the 'maxBSONDepth' server
    // parameter.
    static std::int32_t maxAllowableDepth;

    /**
     * Returns the maximum allowable BSON depth as an unsigned integer. Note that this is a hard
     * limit -- any BSON document that exceeds this limit should be considered invalid.
     */
    static std::uint32_t getMaxAllowableDepth();

    /**
     * Returns the BSON nesting depth limit for stored objects. User documents that exceed this
     * depth are not valid for storage. This limit is slightly lower than the hard limit in
     * getMaxAllowableDepth(), since we may generate things like oplog entries from these documents
     * that contain extra levels of nesting.
     */
    static std::uint32_t getMaxDepthForUserStorage();
};
using BSONDepthIndex = std::uint8_t;
}  // namespace mongo
