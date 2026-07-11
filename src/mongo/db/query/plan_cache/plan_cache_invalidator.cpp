// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/query/plan_cache/plan_cache_invalidator.h"

#include "mongo/db/query/plan_cache/sbe_plan_cache.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

#include <exception>
#include <utility>


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {
namespace {

const auto getCollectionVersionNumber =
    SharedCollectionDecorations::declareDecoration<Atomic<size_t>>();
}  // namespace

PlanCacheInvalidator::PlanCacheInvalidator(const Collection* collection,
                                           ServiceContext* serviceContext)
    : _version{getCollectionVersionNumber(collection->getSharedDecorations()).fetchAndAdd(1u)},
      _uuid{collection->uuid()},
      _serviceContext{serviceContext} {}

PlanCacheInvalidator::~PlanCacheInvalidator() {
    try {
        clearPlanCache();
    } catch (const DBException& ex) {
        LOGV2_WARNING(6006610, "DBException occured on clearing plan cache", "exception"_attr = ex);
    } catch (const std::exception& ex) {
        LOGV2_WARNING(
            6006611, "Exception occured on clearing plan cache", "message"_attr = ex.what());
    } catch (...) {
        LOGV2_WARNING(6006612, "Unknown exception occured on clearing plan cache");
    }
}

void PlanCacheInvalidator::clearPlanCache() const {
    // Some unit tests cannot properly initialize CollectionQueryInfo but rely on it partially
    // initialized to make PlanCacheKeys.
    if (_serviceContext && _uuid) {
        sbe::clearPlanCacheEntriesWith(
            _serviceContext, *_uuid, _version, true /*matchSecondaryCollections*/);
    }
}

}  // namespace mongo
