// bson_collection_catalog_entry.cpp

/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/storage/bson_collection_catalog_entry.h"

#include <algorithm>
#include <numeric>

#include "mongo/db/field_ref.h"

namespace mongo {


BSONCollectionCatalogEntry::BSONCollectionCatalogEntry(StringData ns)
    : CollectionCatalogEntry(ns) {}

// CollectionOptions BSONCollectionCatalogEntry::getCollectionOptions(OperationContext* opCtx) const
// {
//     MetaData md = _getMetaData(opCtx);
//     return md.options;
// }
BSONCollectionCatalogEntry::MetaData BSONCollectionCatalogEntry::getMetaData(
    OperationContext* opCtx) const {
    return _getMetaData(opCtx);
}

int BSONCollectionCatalogEntry::getTotalIndexCount(OperationContext* opCtx) const {
    MetaData md = _getMetaData(opCtx);

    return static_cast<int>(md.indexes.size());
}

int BSONCollectionCatalogEntry::getCompletedIndexCount(OperationContext* opCtx) const {
    MetaData md = _getMetaData(opCtx);

    int num = 0;
    for (unsigned i = 0; i < md.indexes.size(); i++) {
        if (md.indexes[i].ready)
            num++;
    }
    return num;
}

BSONObj BSONCollectionCatalogEntry::getIndexSpec(OperationContext* opCtx,
                                                 StringData indexName) const {
    MetaData md = _getMetaData(opCtx);

    int offset = md.findIndexOffset(indexName);
    invariant(offset >= 0);
    return md.indexes[offset].spec.getOwned();
}


void BSONCollectionCatalogEntry::getAllIndexes(OperationContext* opCtx,
                                               std::vector<std::string>* names) const {
    MetaData md = _getMetaData(opCtx);

    for (unsigned i = 0; i < md.indexes.size(); i++) {
        names->push_back(md.indexes[i].spec["name"].String());
    }
}

void BSONCollectionCatalogEntry::getReadyIndexes(OperationContext* opCtx,
                                                 std::vector<std::string>* names) const {
    MetaData md = _getMetaData(opCtx);

    for (unsigned i = 0; i < md.indexes.size(); i++) {
        if (md.indexes[i].ready)
            names->push_back(md.indexes[i].spec["name"].String());
    }
}

void BSONCollectionCatalogEntry::getAllUniqueIndexes(OperationContext* opCtx,
                                                     std::vector<std::string>* names) const {
    MetaData md = _getMetaData(opCtx);

    for (unsigned i = 0; i < md.indexes.size(); i++) {
        if (md.indexes[i].spec["unique"]) {
            std::string indexName = md.indexes[i].spec["name"].String();
            names->push_back(indexName);
        }
    }
}

bool BSONCollectionCatalogEntry::isIndexMultikey(OperationContext* opCtx,
                                                 StringData indexName,
                                                 MultikeyPaths* multikeyPaths) const {
    MetaData md = _getMetaData(opCtx);

    int offset = md.findIndexOffset(indexName);
    invariant(offset >= 0);

    if (multikeyPaths && !md.indexes[offset].multikeyPaths.empty()) {
        *multikeyPaths = md.indexes[offset].multikeyPaths;
    }

    return md.indexes[offset].multikey;
}

RecordId BSONCollectionCatalogEntry::getIndexHead(OperationContext* opCtx,
                                                  StringData indexName) const {
    MetaData md = _getMetaData(opCtx);

    int offset = md.findIndexOffset(indexName);
    invariant(offset >= 0);
    return md.indexes[offset].head;
}

bool BSONCollectionCatalogEntry::isIndexPresent(OperationContext* opCtx,
                                                StringData indexName) const {
    MetaData md = _getMetaData(opCtx);
    int offset = md.findIndexOffset(indexName);
    return offset >= 0;
}

bool BSONCollectionCatalogEntry::isIndexReady(OperationContext* opCtx, StringData indexName) const {
    MetaData md = _getMetaData(opCtx);

    int offset = md.findIndexOffset(indexName);
    invariant(offset >= 0);
    return md.indexes[offset].ready;
}

KVPrefix BSONCollectionCatalogEntry::getIndexPrefix(OperationContext* opCtx,
                                                    StringData indexName) const {
    MetaData md = _getMetaData(opCtx);
    int offset = md.findIndexOffset(indexName);
    invariant(offset >= 0);
    return md.indexes[offset].prefix;
}

// --------------------------

}  // namespace mongo
