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


#include "mongo/db/query/plan_cache_invalidator.h"

#include "mongo/db/query/sbe_plan_cache.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {
namespace {

const auto getCollectionVersionNumber =
    SharedCollectionDecorations::declareDecoration<AtomicWord<size_t>>();
}  // namespace

PlanCacheInvalidator::PlanCacheInvalidator(const CollectionPtr& collection,
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
