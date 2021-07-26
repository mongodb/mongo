/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/s/sharding_ddl_50_upgrade_downgrade.h"

namespace mongo {

using FeatureCompatibility = ServerGlobalParams::FeatureCompatibility;
using FCVersion = FeatureCompatibility::Version;

DatabaseEntryFormat::Format DatabaseEntryFormat::get(const FixedFCVRegion& fcvRegion) {
    switch (fcvRegion->getVersion()) {
        case FCVersion::kUpgradingFrom44To50:
        case FCVersion::kUpgradingFrom49To50:
        case FCVersion::kVersion50:
            return Format::kUUIDandTimestamp;
        default:
            return Format::kUUIDOnly;
    }
}

ChunkEntryFormat::Format ChunkEntryFormat::get(const FixedFCVRegion& fcvRegion) {
    return getForVersionCallerGuaranteesFCVStability(fcvRegion->getVersion());
}

ChunkEntryFormat::Format ChunkEntryFormat::getForVersionCallerGuaranteesFCVStability(
    ServerGlobalParams::FeatureCompatibility::Version version) {
    switch (version) {
        case FCVersion::kUpgradingFrom44To50:
        case FCVersion::kUpgradingFrom49To50:
            return Format::kNamespaceAndUUIDWithTimestamps;
        case FCVersion::kVersion50:
            return Format::kUUIDOnlyWithTimestamps;
        case FCVersion::kDowngradingFrom50To49:
        case FCVersion::kDowngradingFrom50To44:
            return Format::kNamespaceAndUUIDNoTimestamps;
        case FCVersion::kVersion49:
        case FCVersion::kVersion48:
        case FCVersion::kVersion47:
        case FCVersion::kFullyDowngradedTo44:
            return Format::kNamespaceOnlyNoTimestamps;
        default:
            MONGO_UNREACHABLE;
    }
}

}  // namespace mongo
