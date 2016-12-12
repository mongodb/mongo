/**
 * Copyright (C) 2016 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"

#include "mongo/db/stats/storage_stats.h"

namespace mongo {

Status appendCollectionStorageStats(OperationContext* txn,
                                    const NamespaceString& nss,
                                    const BSONObj& param,
                                    BSONObjBuilder* result) {
    int scale = 1;
    if (param["scale"].isNumber()) {
        scale = param["scale"].numberInt();
        if (scale < 1) {
            return {ErrorCodes::BadValue, "scale has to be >= 1"};
        }
    } else if (param["scale"].trueValue()) {
        return {ErrorCodes::BadValue, "scale has to be a number >= 1"};
    }

    bool verbose = param["verbose"].trueValue();

    AutoGetCollectionForRead ctx(txn, nss);
    if (!ctx.getDb()) {
        return {ErrorCodes::BadValue,
                str::stream() << "Database [" << nss.db().toString() << "] not found."};
    }

    Collection* collection = ctx.getCollection();
    if (!collection) {
        return {ErrorCodes::BadValue,
                str::stream() << "Collection [" << nss.toString() << "] not found."};
    }

    long long size = collection->dataSize(txn) / scale;
    result->appendNumber("size", size);
    long long numRecords = collection->numRecords(txn);
    result->appendNumber("count", numRecords);

    if (numRecords)
        result->append("avgObjSize", collection->averageObjectSize(txn));

    RecordStore* recordStore = collection->getRecordStore();
    result->appendNumber(
        "storageSize",
        static_cast<long long>(recordStore->storageSize(txn, result, verbose ? 1 : 0)) / scale);

    recordStore->appendCustomStats(txn, result, scale);

    IndexCatalog* indexCatalog = collection->getIndexCatalog();
    result->append("nindexes", indexCatalog->numIndexesReady(txn));

    BSONObjBuilder indexDetails;

    IndexCatalog::IndexIterator i = indexCatalog->getIndexIterator(txn, false);
    while (i.more()) {
        const IndexDescriptor* descriptor = i.next();
        IndexAccessMethod* iam = indexCatalog->getIndex(descriptor);
        invariant(iam);

        BSONObjBuilder bob;
        if (iam->appendCustomStats(txn, &bob, scale)) {
            indexDetails.append(descriptor->indexName(), bob.obj());
        }
    }

    result->append("indexDetails", indexDetails.obj());

    BSONObjBuilder indexSizes;
    long long indexSize = collection->getIndexSize(txn, &indexSizes, scale);

    result->appendNumber("totalIndexSize", indexSize / scale);
    result->append("indexSizes", indexSizes.obj());

    return Status::OK();
}
}  // namespace mongo
