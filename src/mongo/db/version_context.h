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

#include "mongo/db/server_options.h"
#include "mongo/db/version_context_metadata_gen.h"

#include <variant>

#include <boost/optional.hpp>

namespace mongo {

class OperationContext;
class ClientLock;

/**
 * The VersionContext holds metadata related to snapshots of the system state taken by Operation
 * Feature Compatibility Version (OFCV)-aware operations.
 *
 * This will help ensure that an operation contacting many nodes will always have the same FCV
 * snapshot. It is stored on the OperationContext and serialized across the network for outgoing
 * requests, ensuring those requests inherit the same FCV snapshot. The VersionContext is also
 * written into any oplog entries to ensure secondaries also see the same snapshot as the original
 * operation.
 *
 * Please note that this is not yet available for all operations - its use is limited to DDL
 * operations for now. Our plans are to expand this to more operations in a future project
 * (SPM-4227).
 */
class VersionContext {
public:
    using FCV = multiversion::FeatureCompatibilityVersion;
    using FCVSnapshot = ServerGlobalParams::FCVSnapshot;

    struct OperationWithoutOFCVTag {};
    struct OutsideOperationTag {};
    struct IgnoreOFCVTag {};

    static const VersionContext& getDecoration(const OperationContext* opCtx);
    /**
     * Sets the VersionContext decoration over the OperationContext.
     * The Client lock must be taken to serialize with concurrent readers, such as $currentOp.
     */
    static void setDecoration(ClientLock&, OperationContext* opCtx, const VersionContext& vCtx);
    static void setFromMetadata(ClientLock&, OperationContext* opCtx, const VersionContext& vCtx);

    /**
     * Sets the VersionContext decoration over the OperationContext over a given scope.
     * At the end of the scope, the VersionContext decoration is reset to its default state
     * (i.e. operation without Operation FCV).
     * The Client lock over the opCtx's client is automatically taken on creation and destruction.
     */
    class ScopedSetDecoration {
    public:
        ScopedSetDecoration(OperationContext* opCtx, const VersionContext& vCtx);
        ScopedSetDecoration(const ScopedSetDecoration&) = delete;
        ScopedSetDecoration(ScopedSetDecoration&&) = delete;
        ~ScopedSetDecoration();

    private:
        OperationContext* _opCtx;
    };

    /**
     * Sets the current FCV as the VersionContext decoration over the passed in operation context
     * (unless an OFCV is already set, in that case the scoped object has no effect). While the
     * object is in scope, the operation context will have a stable view of feature flags. The
     * Client lock over the opCtx's client is automatically taken on creation and destruction.
     */
    class FixedOperationFCVRegion {
    public:
        FixedOperationFCVRegion(OperationContext* opCtx);
        FixedOperationFCVRegion(const FixedOperationFCVRegion&) = delete;
        FixedOperationFCVRegion(FixedOperationFCVRegion&&) = delete;
        ~FixedOperationFCVRegion();

    private:
        OperationContext* _opCtx;
    };

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

    void resetToOperationWithoutOFCV();

    inline bool hasOperationFCV() const {
        return std::holds_alternative<VersionContextMetadata>(_metadataOrTag);
    }

    class Passkey {
    private:
        friend class VersionContextTest;
        friend class FCVGatedFeatureFlag;
        friend class ShardingDDLCoordinator;
        Passkey() = default;
        ~Passkey() = default;
    };

    // To prevent potential misuse or confusion in the codebase regarding the distinction
    // between operation-specific and global FCV contexts, access to the getOperationFCV()
    // method is restricted. This restriction is implemented using the Passkey pattern,
    // such that only specific classes can construct an instance of the passkey and,
    // consequently, call the restricted getOperationFCV() method.
    inline boost::optional<FCVSnapshot> getOperationFCV(Passkey) const {
        if (auto* metadata = std::get_if<VersionContextMetadata>(&_metadataOrTag)) {
            return boost::optional<FCVSnapshot>{metadata->getOFCV()};
        }
        return boost::none;
    }

    BSONObj toBSON() const;

    friend bool operator==(const VersionContext& lhs, const VersionContext& rhs);

    bool canPropagateAcrossShards() const {
        return _canPropagateAcrossShards;
    }

    /**
     * If true, this instance can be serialized in network commands to shards in a sharded cluster.
     *
     * It may only be enabled by durable operations (e.g. ShardingDDLCoordinator), which setFCV can
     * reliably track and drain. In contrast, standalone operations can not safely enable this flag,
     * because it may be killed, and setFCV is unaware that it may still be serialized in a command.
     *
     * Misusing this API can lead to a subtle failure mode, where a delayed/replayed network command
     * can arrive after a FCV transition, and perform a write with a wrong (stale) OFCV, and persist
     * inconsistent data as a result.
     *
     * Please include a Catalog and Routing member on the code review before enabling this flag.
     * This flag is only for safety and is not considered in comparisons or BSON serialization.
     */
    [[nodiscard]] VersionContext withPropagationAcrossShards_UNSAFE() const {
        invariant(std::holds_alternative<VersionContextMetadata>(_metadataOrTag));
        VersionContext copy = *this;
        copy._canPropagateAcrossShards = true;
        return copy;
    }

private:
    void _assertHasNoOperationFCV() const;

    bool _isMatchingOFCV(FCV fcv) const;

    std::
        variant<OperationWithoutOFCVTag, OutsideOperationTag, IgnoreOFCVTag, VersionContextMetadata>
            _metadataOrTag;

    bool _canPropagateAcrossShards = false;
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
inline const VersionContext kVersionContextIgnored_UNSAFE{VersionContext::IgnoreOFCVTag{}};

/**
 * Synchronously wait for all operations associated with a stale version context to drain.
 * It makes sense to call this function only when it is guaranteed that no new operations
 * with a different version context can start (e.g. following an FCV transition).
 *
 * It does NOT wait for operations that are:
 * - Associated with the passed in version context
 * - Not associated with a version context at all
 * - Associated with a stale version context, but that have been already killed
 *
 */
void waitForOperationsNotMatchingVersionContextToComplete(OperationContext* opCtx,
                                                          const VersionContext& vCtx,
                                                          Date_t deadline = Date_t::max());

}  // namespace mongo
