/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include <fmt/format.h>
#include <utility>
#include <variant>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/projection.h"
#include "mongo/db/exec/write_stage_common.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collation_index_key.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/index_bounds_builder.h"
#include "mongo/db/query/plan_explainer_express.h"
#include "mongo/db/query/projection.h"
#include "mongo/db/record_id.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/s/scoped_collection_metadata.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/storage/write_unit_of_work.h"

namespace mongo {
namespace express {

/**
 * We encountered a situation where the record referenced by the index entry is gone. Check whether
 * or not this read is ignoring prepare conflicts, then log and/or error appropriately.
 */
void logRecordNotFound(OperationContext* opCtx,
                       const RecordId& rid,
                       const BSONObj& indexKey,
                       const BSONObj& keyPattern,
                       const NamespaceString& ns);

/**
 * The 'PlanProgress' variant (defined below) represents the possible return values from an
 * execution step in 'ExpressPlan' execution. It holds one of the following results.
 */

/**
 * Execution made forward progress.
 */
class Ready {
public:
    static constexpr bool indicatesSuccessfulProgress = true;
};

/**
 * Execution completed successfully, and there is no remaining work. Note that execution can
 * return 'Exhausted' when it produces its last document.
 *
 * It is safe to make additional calls to the 'ExpressPlan::proceed' function, but they will always
 * return 'Exhausted' and will never produce any more documents.
 */
class Exhausted {
public:
    static constexpr bool indicatesSuccessfulProgress = true;
};

using PlanProgress = std::variant<Ready, Exhausted>;

inline bool isSuccessfulResult(const PlanProgress& result) {
    return std::visit([](const auto& value) { return value.indicatesSuccessfulProgress; }, result);
}

/**
 * A document iterator that uses a collection's _id index to iterate over documents in a collection
 * that match a simple equality predicate on their _id field. The _id index is required to be
 * unique, so the iterator will produce at most one document.
 */
class IdLookupViaIndex {
public:
    IdLookupViaIndex(const BSONObj& queryFilter) : _queryFilter(queryFilter) {}

    void open(OperationContext* opCtx, const CollectionPtr* collection, IteratorStats* stats) {
        _collection = collection;
        _indexCatalogEntry = IdLookupViaIndex::getIndexCatalogEntryForIdIndex(opCtx, *collection);

        _stats = stats;
        _stats->setStageName("EXPRESS_IXSCAN");
        _stats->setIndexName("_id_");
        _stats->setIndexKeyPattern(BSON("_id" << 1));
    }

    template <class Continuation>
    PlanProgress consumeOne(OperationContext* opCtx, Continuation continuation) {
        if (_exhausted) {
            return Exhausted();
        }

        auto rid = _indexCatalogEntry->accessMethod()->asSortedData()->findSingle(
            opCtx, *_collection, _indexCatalogEntry, _queryFilter["_id"].wrap());
        if (rid.isNull()) {
            _exhausted = true;
            return Exhausted();
        }
        _stats->incNumKeysExamined(1);

        Snapshotted<BSONObj> obj;
        bool found = (*_collection)->findDoc(opCtx, rid, &obj);
        if (!found) {
            logRecordNotFound(opCtx,
                              rid,
                              _queryFilter,
                              _indexCatalogEntry->descriptor()->keyPattern(),
                              _collection->get()->ns());
            _exhausted = true;
            return Exhausted();
        }

        _stats->incNumDocumentsFetched(1);

        auto progress = continuation(*_collection, std::move(rid), std::move(obj));

        // Only advance the iterator if the continuation completely processed its item, as indicated
        // by its return value.
        if (isSuccessfulResult(progress)) {
            _exhausted = true;
            return Exhausted{};
        }

        return progress;
    }

    bool exhausted() const {
        return _exhausted;
    };

    void releaseResources() {
        _collection = nullptr;
        _indexCatalogEntry = nullptr;
    }

    void restoreResources(OperationContext* opCtx, const CollectionPtr* collection) {
        _collection = collection;
        _indexCatalogEntry = IdLookupViaIndex::getIndexCatalogEntryForIdIndex(opCtx, *collection);
    }

private:
    static const IndexCatalogEntry* getIndexCatalogEntryForIdIndex(
        OperationContext* opCtx, const CollectionPtr& collection) {
        const IndexCatalog* catalog = collection->getIndexCatalog();
        const IndexDescriptor* desc = catalog->findIdIndex(opCtx);
        tassert(8884401, "Missing _id index on non-clustered collection", desc);

        return catalog->getEntry(desc);
    }

    BSONObj _queryFilter;  // Unowned BSON.

    const CollectionPtr* _collection{nullptr};             // Unowned.
    const IndexCatalogEntry* _indexCatalogEntry{nullptr};  // Unowned.

    bool _exhausted{false};

    IteratorStats* _stats{nullptr};
};

/**
 * A document iterator for collections clustered by the _id field that directly iterates documents
 * matching a simple equality predicate on their _id field. The _id field is required to be unique,
 * so the iterator will produce at most one document.
 */
class IdLookupOnClusteredCollection {
public:
    IdLookupOnClusteredCollection(const BSONObj& queryFilter) : _queryFilter(queryFilter) {}

    void open(OperationContext* opCtx, const CollectionPtr* collection, IteratorStats* stats) {
        _collection = collection;
        _stats = stats;
        _stats->setStageName("EXPRESS_CLUSTERED_IXSCAN");
    }

    template <class Continuation>
    PlanProgress consumeOne(OperationContext* opCtx, Continuation continuation) {
        if (_exhausted) {
            return Exhausted();
        }

        auto rid = record_id_helpers::keyForObj(IndexBoundsBuilder::objFromElement(
            _queryFilter["_id"], (*_collection)->getDefaultCollator()));

        Snapshotted<BSONObj> obj;
        bool found = (*_collection)->findDoc(opCtx, rid, &obj);
        if (!found) {
            _exhausted = true;
            return Exhausted();
        }
        _stats->incNumDocumentsFetched(1);

        auto progress = continuation(*_collection, std::move(rid), std::move(obj));

        // Only advance the iterator if the continuation completely processed its item, as indicated
        // by its return value.
        if (isSuccessfulResult(progress)) {
            _exhausted = true;
            return Exhausted();
        }

        return progress;
    }

    bool exhausted() const {
        return _exhausted;
    };

    void releaseResources() {
        _collection = nullptr;
    }

    void restoreResources(OperationContext* opCtx, const CollectionPtr* collection) {
        _collection = collection;
    }

private:
    BSONObj _queryFilter;  // Unowned BSON.

    const CollectionPtr* _collection{nullptr};

    bool _exhausted{false};

    IteratorStats* _stats{nullptr};
};

class LookupViaUserIndex {
public:
    LookupViaUserIndex(const BSONElement& filterValue,
                       std::string indexIdent,
                       std::string indexName,
                       const CollatorInterface* collator)
        : _filterValue(filterValue),
          _indexIdent(std::move(indexIdent)),
          _indexName(std::move(indexName)),
          _collator(collator) {}

    void open(OperationContext* opCtx, const CollectionPtr* collection, IteratorStats* stats) {
        _collection = collection;
        _indexCatalogEntry = LookupViaUserIndex::getIndexCatalogEntryForUserIndex(
            opCtx, *collection, _indexIdent, _indexName);

        _stats = stats;
        _stats->setStageName("EXPRESS_IXSCAN");
        _stats->setIndexName(_indexName);
        _stats->setIndexKeyPattern(_indexCatalogEntry->descriptor()->keyPattern());
    }

    template <class Continuation>
    PlanProgress consumeOne(OperationContext* opCtx, Continuation continuation) {
        if (_exhausted) {
            return Exhausted();
        }

        // Build the start and end bounds for the equality by appending a fully-open bound for each
        // remaining field in the compound index.
        BSONObjBuilder startBob, endBob;
        CollationIndexKey::collationAwareIndexKeyAppend(_filterValue, _collator, &startBob);
        CollationIndexKey::collationAwareIndexKeyAppend(_filterValue, _collator, &endBob);
        auto desc = _indexCatalogEntry->descriptor();
        for (int i = 1; i < desc->getNumFields(); ++i) {
            if (desc->ordering().get(i) == 1) {
                startBob.appendMinKey("");
                endBob.appendMaxKey("");
            } else {
                startBob.appendMaxKey("");
                endBob.appendMinKey("");
            }
        }
        auto startKey = startBob.obj();
        auto endKey = endBob.obj();

        // Now seek to the first matching key in the index.
        auto sortedAccessMethod = _indexCatalogEntry->accessMethod()->asSortedData();
        auto indexCursor = sortedAccessMethod->newCursor(opCtx, true /* forward */);
        indexCursor->setEndPosition(endKey, true /* endKeyInclusive */);
        auto keyStringForSeek = IndexEntryComparison::makeKeyStringFromBSONKeyForSeek(
            startKey,
            sortedAccessMethod->getSortedDataInterface()->getKeyStringVersion(),
            sortedAccessMethod->getSortedDataInterface()->getOrdering(),
            true /* forward */,
            true /* startKeyInclusive */);

        auto keyEntry = indexCursor->seekForKeyValueView(keyStringForSeek);
        if (keyEntry.isEmpty()) {
            _exhausted = true;
            return Exhausted();
        }
        _stats->incNumKeysExamined(1);

        auto rid = keyEntry.getRecordId();
        tassert(8884402, "Index entry with null record id", rid && !rid->isNull());

        Snapshotted<BSONObj> obj;
        bool found = (*_collection)->findDoc(opCtx, *rid, &obj);
        if (!found) {
            const auto& keyPattern = _indexCatalogEntry->descriptor()->keyPattern();
            auto dehydratedKp = key_string::toBson(keyEntry.getKeyStringWithoutRecordIdView(),
                                                   Ordering::make(keyPattern),
                                                   keyEntry.getTypeBitsView(),
                                                   keyEntry.getVersion());

            logRecordNotFound(opCtx,
                              *rid,
                              IndexKeyEntry::rehydrateKey(keyPattern, dehydratedKp),
                              keyPattern,
                              _collection->get()->ns());
            return Ready();
        }

        _stats->incNumDocumentsFetched(1);

        auto progress = continuation(*_collection, *rid, std::move(obj));

        // Only advance the iterator if the continuation completely processed its item, as indicated
        // by its return value.
        if (isSuccessfulResult(progress)) {
            _exhausted = true;
            return Exhausted{};
        }

        return progress;
    }

    bool exhausted() const {
        return _exhausted;
    };

    void releaseResources() {
        _collection = nullptr;
        _indexCatalogEntry = nullptr;
    }

    void restoreResources(OperationContext* opCtx, const CollectionPtr* collection) {
        _collection = collection;
        _indexCatalogEntry = LookupViaUserIndex::getIndexCatalogEntryForUserIndex(
            opCtx, *collection, _indexIdent, _indexName);
    }

private:
    static const IndexCatalogEntry* getIndexCatalogEntryForUserIndex(
        OperationContext* opCtx,
        const CollectionPtr& collection,
        const std::string& indexIdent,
        const std::string& indexName) {
        const IndexCatalog* catalog = collection->getIndexCatalog();
        const IndexDescriptor* desc = catalog->findIndexByIdent(opCtx, indexIdent);
        uassert(ErrorCodes::QueryPlanKilled,
                fmt::format("query plan killed :: index {} dropped", indexName),
                desc);

        return catalog->getEntry(desc);
    }

    BSONElement _filterValue;  // Unowned BSON.
    const std::string _indexIdent;
    const std::string _indexName;

    const CollectionPtr* _collection{nullptr};             // Unowned.
    const IndexCatalogEntry* _indexCatalogEntry{nullptr};  // Unowned.

    const CollatorInterface* _collator;  // Owned by the query's ExpressionContext.

    bool _exhausted{false};

    IteratorStats* _stats{nullptr};
};

class NoShardFilter {};

template <class Continuation>
PlanProgress applyShardFilter(NoShardFilter&,
                              const Snapshotted<BSONObj>&,
                              const NamespaceString&,
                              Continuation continuation) {
    bool shouldWriteToOrphan = false;
    return continuation(shouldWriteToOrphan);
}

template <class Continuation>
PlanProgress applyShardFilter(ScopedCollectionFilter& collectionFilter,
                              const Snapshotted<BSONObj>& obj,
                              const NamespaceString&,
                              Continuation continuation) {
    bool accepted = [&]() {
        if (!collectionFilter.isSharded()) {
            return true;
        }

        auto shardKey = collectionFilter.getShardKeyPattern().extractShardKeyFromDoc(obj.value());
        return !shardKey.isEmpty() && collectionFilter.keyBelongsToMe(shardKey);
    }();

    if (accepted) {
        bool shouldWriteToOrphan = false;
        return continuation(shouldWriteToOrphan);
    } else {
        // Indicate that we have made progress on plan execution but have not produced a document.
        return Ready();
    }
}

class IdentityProjection {};

template <class Continuation>
PlanProgress applyProjection(IdentityProjection, BSONObj obj, Continuation continuation) {
    // Presto change-o!
    return continuation(std::move(obj));
}

template <class Continuation>
auto applyProjection(const projection_ast::Projection* projection,
                     BSONObj obj,
                     Continuation continuation) {
    auto projType = projection->type();
    if (projType == projection_ast::ProjectType::kInclusion) {
        return continuation(
            ProjectionStageSimple::transform(obj, projection->getRequiredFields(), projType));
    } else {
        return continuation(
            ProjectionStageSimple::transform(obj, projection->getExcludedPaths(), projType));
    }
}

/**
 * ExpressPlan is a streamlined execution engine that supports one specific sequence of query
 * stages.
 *   - Document iterator -> optional shard filter -> optional write -> optional projection
 *
 * Each stage has multiple implementations which are specified as template parameters. Optional
 * stages can be disabled by specifying a no-op implementation.
 */
template <class IteratorChoice, class ShardFilterChoice, class ProjectionChoice>
class ExpressPlan {
public:
    ExpressPlan(IteratorChoice iterator, ShardFilterChoice shardFilter, ProjectionChoice projection)
        : _iterator(std::move(iterator)),
          _shardFilter(std::move(shardFilter)),
          _projection(std::move(projection)) {}

    void open(OperationContext* opCtx,
              const CollectionPtr* collection,
              PlanStats* planStats,
              IteratorStats* iteratorStats) {
        _planStats = planStats;
        _iterator.open(opCtx, collection, iteratorStats);
    }

    template <class Continuation>
    PlanProgress proceed(OperationContext* opCtx, Continuation continuation) {
        return _iterator.consumeOne(
            opCtx, [&](const CollectionPtr& collection, RecordId rid, Snapshotted<BSONObj> obj) {
                // Continue execution with one (rid, obj) pair from the iterator.
                return applyShardFilter(  //
                    _shardFilter,
                    obj,
                    collection->ns(),
                    [&](bool shouldWriteToOrphan) {
                        // Continue execution when the document is accepted by the filter.
                        return applyProjection(  //
                            _projection,
                            std::move(obj.value()),
                            [&](BSONObj projectionResult) {
                                // Continue execution with the result of applying the
                                // projection.
                                _planStats->incNumResults(1);
                                return continuation(std::move(rid), std::move(projectionResult));
                            });
                    });
            });
    }

    void releaseResources() {
        _iterator.releaseResources();
    }

    void restoreResources(OperationContext* opCtx, const CollectionPtr* collection) {
        _iterator.restoreResources(opCtx, collection);
    }

    bool exhausted() const {
        return _iterator.exhausted();
    }

private:
    IteratorChoice _iterator;
    ShardFilterChoice _shardFilter;
    ProjectionChoice _projection;

    PlanStats* _planStats{nullptr};
};
}  // namespace express
}  // namespace mongo
