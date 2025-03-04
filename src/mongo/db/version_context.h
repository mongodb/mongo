/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include <boost/optional.hpp>
#include <variant>

#include "mongo/db/server_options.h"
#include "mongo/db/version_context_metadata_gen.h"

namespace mongo {

class OperationContext;

/**
 * The VersionContext holds metadata related to snapshots of the system state
 * taken by OFCV-aware operations.
 */
class VersionContext {
public:
    using FCV = multiversion::FeatureCompatibilityVersion;
    using FCVSnapshot = ServerGlobalParams::FCVSnapshot;

    struct OperationWithoutOFCVTag {};
    struct OutsideOperationTag {};
    struct IgnoreOFCVTag {};

    static VersionContext& getDecoration(OperationContext*);

    constexpr VersionContext() : _metadataOrTag(OperationWithoutOFCVTag{}) {}
    explicit constexpr VersionContext(OutsideOperationTag tag) : _metadataOrTag(tag) {}
    explicit constexpr VersionContext(IgnoreOFCVTag tag) : _metadataOrTag(tag) {}

    explicit VersionContext(FCV fcv);

    explicit VersionContext(FCVSnapshot fcv);

    explicit VersionContext(const BSONObj& bsonObject);

    VersionContext(const VersionContext& other) = default;

    VersionContext& operator=(const VersionContext& other);

    void setOperationFCV(FCV fcv);

    void setOperationFCV(FCVSnapshot fcv);

    inline boost::optional<FCVSnapshot> getOperationFCV() const {
        if (auto* metadata = std::get_if<VersionContextMetadata>(&_metadataOrTag)) {
            return boost::optional<FCVSnapshot>{metadata->getOFCV()};
        }
        return boost::none;
    }

    BSONObj toBSON() const;

    friend bool operator==(const VersionContext& lhs, const VersionContext& rhs);

private:
    void _assertOFCVNotInitialized() const;

    bool _isMatchingOFCV(FCV fcv) const;

    std::
        variant<OperationWithoutOFCVTag, OutsideOperationTag, IgnoreOFCVTag, VersionContextMetadata>
            _metadataOrTag;
};

/**
 * Use this when running outside of an operation (for example, during startup, or in unit tests).
 */
inline const VersionContext kNoVersionContext{VersionContext::OutsideOperationTag{}};

/**
 * Use this if you want to deliberately bypass Operation FCV and make feature flag checks against
 * the node's local FCV only. This should be used with a lot of care, only if you can ensure none
 * of your current or future callers acts incorrectly due to ignoring their Operation FCV.
 */
inline const VersionContext kVersionContextIgnored{VersionContext::IgnoreOFCVTag{}};

}  // namespace mongo
