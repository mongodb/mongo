// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/classic/requires_collection_stage.h"
#include "mongo/db/query/all_indices_required_checker.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {

/**
 * A base class for plan stages which require access to _all_ indices of a collection, and should
 * cause the query to die on yield recovery if any index is dropped. Plan stages which depend on a
 * single index, such as IXSCAN, should instead use RequiresIndexStage.
 */
class RequiresAllIndicesStage : public RequiresCollectionStage {
public:
    RequiresAllIndicesStage(std::string_view stageType,
                            ExpressionContext* expCtx,
                            CollectionAcquisition coll)
        : RequiresCollectionStage(stageType, expCtx, coll) {
        auto multipleCollection = MultipleCollectionAccessor{collection()};
        _allIndicesRequiredChecker.emplace(std::move(multipleCollection));
    }

    ~RequiresAllIndicesStage() override = default;

protected:
    void doSaveStateRequiresCollection() final {}

    void doRestoreStateRequiresCollection() final {
        if (_allIndicesRequiredChecker) {
            auto multipleCollection = MultipleCollectionAccessor{collection()};

            _allIndicesRequiredChecker->check(opCtx(), multipleCollection);
        }
    }

    /**
     * Subclasses may call this to indicate that they no longer require all indices on the
     * collection to survive. After calling this, yield recovery will never fail.
     */
    void releaseAllIndicesRequirement() {
        _allIndicesRequiredChecker = boost::none;
    }

private:
    boost::optional<AllIndicesRequiredChecker> _allIndicesRequiredChecker;
};

}  // namespace mongo
