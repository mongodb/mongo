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

#include "mongo/db/exec/classic/requires_collection_stage.h"
#include "mongo/db/query/all_indices_required_checker.h"
#include "mongo/db/query/multiple_collection_accessor.h"

namespace mongo {

/**
 * A base class for plan stages which require access to _all_ indices of a collection, and should
 * cause the query to die on yield recovery if any index is dropped. Plan stages which depend on a
 * single index, such as IXSCAN, should instead use RequiresIndexStage.
 */
class RequiresAllIndicesStage : public RequiresCollectionStage {
public:
    RequiresAllIndicesStage(const char* stageType,
                            ExpressionContext* expCtx,
                            VariantCollectionPtrOrAcquisition collectionVariant)
        : RequiresCollectionStage(stageType, expCtx, collectionVariant) {
        const auto& coll = collection();
        auto multipleCollection = coll.isAcquisition()
            ? MultipleCollectionAccessor{coll.getAcquisition()}
            : MultipleCollectionAccessor{coll.getCollectionPtr()};
        _allIndicesRequiredChecker.emplace(std::move(multipleCollection));
    }

    ~RequiresAllIndicesStage() override = default;

protected:
    void doSaveStateRequiresCollection() final {}

    void doRestoreStateRequiresCollection() final {
        if (_allIndicesRequiredChecker) {
            const auto& coll = collection();
            auto multipleCollection = coll.isAcquisition()
                ? MultipleCollectionAccessor{coll.getAcquisition()}
                : MultipleCollectionAccessor{coll.getCollectionPtr()};

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
