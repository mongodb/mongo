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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kIndex

#include "mongo/platform/basic.h"

#include "mongo/db/index/index_build_interceptor.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/multi_key_path_tracker.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/util/log.h"
#include "mongo/util/uuid.h"

namespace mongo {

namespace {
const bool makeCollections = false;
}

NamespaceString IndexBuildInterceptor::makeTempSideWritesNs() {
    return NamespaceString("local.system.sideWrites-" + UUID::gen().toString());
}

void IndexBuildInterceptor::ensureSideWritesCollectionExists(OperationContext* opCtx) {
    if (!makeCollections) {
        return;
    }

    // TODO SERVER-38027 Consider pushing this higher into the createIndexes command logic.
    OperationShardingState::get(opCtx).setAllowImplicitCollectionCreation(BSONElement());

    AutoGetOrCreateDb local(opCtx, "local", LockMode::MODE_X);
    CollectionOptions options;
    options.setNoIdIndex();
    options.temp = true;

    local.getDb()->createCollection(opCtx, _sideWritesNs.ns(), options);
}

void IndexBuildInterceptor::removeSideWritesCollection(OperationContext* opCtx) {
    if (!makeCollections) {
        return;
    }

    AutoGetDb local(opCtx, "local", LockMode::MODE_X);
    fassert(50994, local.getDb()->dropCollectionEvenIfSystem(opCtx, _sideWritesNs, repl::OpTime()));
}

Status IndexBuildInterceptor::sideWrite(OperationContext* opCtx,
                                        IndexAccessMethod* indexAccessMethod,
                                        const BSONObj* obj,
                                        RecordId loc,
                                        Op op,
                                        int64_t* numKeysOut) {
    *numKeysOut = 0;
    BSONObjSet keys = SimpleBSONObjComparator::kInstance.makeBSONObjSet();
    BSONObjSet multikeyMetadataKeys = SimpleBSONObjComparator::kInstance.makeBSONObjSet();
    MultikeyPaths multikeyPaths;

    indexAccessMethod->getKeys(*obj,
                               IndexAccessMethod::GetKeysMode::kEnforceConstraints,
                               &keys,
                               &multikeyMetadataKeys,
                               &multikeyPaths);
    // Maintain parity with IndexAccessMethods handling of key counting. Only include
    // `multikeyMetadataKeys` when inserting.
    *numKeysOut = keys.size() + (op == Op::kInsert ? multikeyMetadataKeys.size() : 0);

    if (_multikeyPaths) {
        MultikeyPathTracker::mergeMultikeyPaths(&_multikeyPaths.get(), multikeyPaths);
    } else {
        // `mergeMultikeyPaths` is sensitive to the two inputs having the same multikey
        // "shape". Initialize `_multikeyPaths` with the right shape from the first result.
        _multikeyPaths = multikeyPaths;
    }

    AutoGetCollection coll(opCtx, _sideWritesNs, LockMode::MODE_IX);
    invariant(coll.getCollection());

    std::vector<InsertStatement> toInsert;
    for (const auto& key : keys) {
        // Documents inserted into this table must be consumed in insert-order. Today, we can rely
        // on storage engines to return documents in insert-order, but with clustered indexes,
        // that may no longer be true.
        //
        // Additionally, these writes should be timestamped with the same timestamps that the
        // other writes making up this operation are given. When index builds can cope with
        // replication rollbacks, side table writes associated with a CUD operation should
        // remain/rollback along with the corresponding oplog entry.
        toInsert.emplace_back(BSON(
            "op" << (op == Op::kInsert ? "i" : "d") << "key" << key << "recordId" << loc.repr()));
    }

    if (op == Op::kInsert) {
        // Wildcard indexes write multikey path information, typically part of the catalog
        // document, to the index itself. Multikey information is never deleted, so we only need
        // to add this data on the insert path.
        for (const auto& key : multikeyMetadataKeys) {
            toInsert.emplace_back(BSON("op"
                                       << "i"
                                       << "key"
                                       << key
                                       << "recordId"
                                       << static_cast<int64_t>(
                                              RecordId::ReservedId::kWildcardMultikeyMetadataId)));
        }
    }

    OpDebug* const opDebug = nullptr;
    const bool fromMigrate = false;
    return coll.getCollection()->insertDocuments(
        opCtx, toInsert.begin(), toInsert.end(), opDebug, fromMigrate);
}
}  // namespace mongo
