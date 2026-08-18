// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <boost/optional/optional.hpp>

namespace mongo {
namespace metadata_consistency_internal {

/**
 * Allows optimistic checks under the assumption that a FCV-gated feature flag is stable.
 * This does not prevent the feature flag from changing, it merely allows detecting the change.
 * Unlike two feature flag checks, this class detects a full FCV upgrade+downgrade (ABA problem).
 *
 * TODO(SERVER-98118): remove once 9.0 is last LTS
 */
class [[MONGO_MOD_PARENT_PRIVATE]] OptimisticFCVFeatureFlagGuard {
public:
    explicit OptimisticFCVFeatureFlagGuard(OperationContext* opCtx,
                                           FCVGatedFeatureFlag& featureFlag)
        : _opCtx(opCtx), _featureFlag(featureFlag) {
        const auto initialFcvDoc = readFCVDocument(opCtx);
        _initialEnabled = _featureFlag.isEnabledOnVersion(initialFcvDoc.getVersion());
        _initialChangeTimestamp = initialFcvDoc.getChangeTimestamp();
    }

    bool wasEnabled() const {
        return _initialEnabled;
    }

    // Returns true if the feature flag state and FCV change timestamp are unchanged, and false
    // otherwise.
    bool validateUnchanged() const {
        const auto currentFcvDoc = readFCVDocument(_opCtx);
        return _featureFlag.isEnabledOnVersion(currentFcvDoc.getVersion()) == _initialEnabled &&
            currentFcvDoc.getChangeTimestamp() == _initialChangeTimestamp;
    }

private:
    static FeatureCompatibilityVersionDocument readFCVDocument(OperationContext* opCtx) {
        return FeatureCompatibilityVersionDocument::parse(uassertStatusOK(
            FeatureCompatibilityVersion::findFeatureCompatibilityVersionDocument(opCtx)));
    }

    OperationContext* _opCtx;
    FCVGatedFeatureFlag& _featureFlag;
    bool _initialEnabled;
    boost::optional<Timestamp> _initialChangeTimestamp;
};

}  // namespace metadata_consistency_internal
}  // namespace mongo
