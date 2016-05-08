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

#include "mongo/db/field_ref.h"

namespace mongo {

namespace {

// An index will fail to get created if the size in bytes of its key pattern is greater than 2048.
// We use that value to represent the largest number of path components we could ever possibly
// expect to see in an indexed field.
const size_t kMaxKeyPatternPathLength = 2048;
char multikeyPathsEncodedAsBytes[kMaxKeyPatternPathLength];

/**
 * Encodes 'multikeyPaths' as binary data and appends it to 'bob'.
 *
 * For example, consider the index {'a.b': 1, 'a.c': 1} where the paths "a" and "a.b" cause it to be
 * multikey. The object {'a.b': HexData('0101'), 'a.c': HexData('0100')} would then be appended to
 * 'bob'.
 */
void appendMultikeyPathsAsBytes(BSONObj keyPattern,
                                const MultikeyPaths& multikeyPaths,
                                BSONObjBuilder* bob) {
    size_t i = 0;
    for (const auto keyElem : keyPattern) {
        StringData keyName = keyElem.fieldNameStringData();
        size_t numParts = FieldRef{keyName}.numParts();
        invariant(numParts > 0);
        invariant(numParts <= kMaxKeyPatternPathLength);

        std::fill_n(multikeyPathsEncodedAsBytes, numParts, 0);
        for (const auto multikeyComponent : multikeyPaths[i]) {
            multikeyPathsEncodedAsBytes[multikeyComponent] = 1;
        }
        bob->appendBinData(keyName, numParts, BinDataGeneral, &multikeyPathsEncodedAsBytes[0]);

        ++i;
    }
}

/**
 * Parses the path-level multikey information encoded as binary data from 'multikeyPathsObj' and
 * sets 'multikeyPaths' as that value.
 *
 * For example, consider the index {'a.b': 1, 'a.c': 1} where the paths "a" and "a.b" cause it to be
 * multikey. The binary data {'a.b': HexData('0101'), 'a.c': HexData('0100')} would then be parsed
 * into std::vector<std::set<size_t>>{{0U, 1U}, {0U}}.
 */
void parseMultikeyPathsFromBytes(BSONObj multikeyPathsObj, MultikeyPaths* multikeyPaths) {
    invariant(multikeyPaths);
    for (auto elem : multikeyPathsObj) {
        std::set<size_t> multikeyComponents;
        int len;
        const char* data = elem.binData(len);
        invariant(len > 0);
        invariant(static_cast<size_t>(len) <= kMaxKeyPatternPathLength);

        for (int i = 0; i < len; ++i) {
            if (data[i]) {
                multikeyComponents.insert(i);
            }
        }
        multikeyPaths->push_back(multikeyComponents);
    }
}

}  // namespace

BSONCollectionCatalogEntry::BSONCollectionCatalogEntry(StringData ns)
    : CollectionCatalogEntry(ns) {}

CollectionOptions BSONCollectionCatalogEntry::getCollectionOptions(OperationContext* txn) const {
    MetaData md = _getMetaData(txn);
    return md.options;
}

int BSONCollectionCatalogEntry::getTotalIndexCount(OperationContext* txn) const {
    MetaData md = _getMetaData(txn);

    return static_cast<int>(md.indexes.size());
}

int BSONCollectionCatalogEntry::getCompletedIndexCount(OperationContext* txn) const {
    MetaData md = _getMetaData(txn);

    int num = 0;
    for (unsigned i = 0; i < md.indexes.size(); i++) {
        if (md.indexes[i].ready)
            num++;
    }
    return num;
}

BSONObj BSONCollectionCatalogEntry::getIndexSpec(OperationContext* txn,
                                                 StringData indexName) const {
    MetaData md = _getMetaData(txn);

    int offset = md.findIndexOffset(indexName);
    invariant(offset >= 0);
    return md.indexes[offset].spec.getOwned();
}


void BSONCollectionCatalogEntry::getAllIndexes(OperationContext* txn,
                                               std::vector<std::string>* names) const {
    MetaData md = _getMetaData(txn);

    for (unsigned i = 0; i < md.indexes.size(); i++) {
        names->push_back(md.indexes[i].spec["name"].String());
    }
}

bool BSONCollectionCatalogEntry::isIndexMultikey(OperationContext* txn,
                                                 StringData indexName,
                                                 MultikeyPaths* multikeyPaths) const {
    MetaData md = _getMetaData(txn);

    int offset = md.findIndexOffset(indexName);
    invariant(offset >= 0);

    if (multikeyPaths && !md.indexes[offset].multikeyPaths.empty()) {
        *multikeyPaths = md.indexes[offset].multikeyPaths;
    }

    return md.indexes[offset].multikey;
}

RecordId BSONCollectionCatalogEntry::getIndexHead(OperationContext* txn,
                                                  StringData indexName) const {
    MetaData md = _getMetaData(txn);

    int offset = md.findIndexOffset(indexName);
    invariant(offset >= 0);
    return md.indexes[offset].head;
}

bool BSONCollectionCatalogEntry::isIndexReady(OperationContext* txn, StringData indexName) const {
    MetaData md = _getMetaData(txn);

    int offset = md.findIndexOffset(indexName);
    invariant(offset >= 0);
    return md.indexes[offset].ready;
}

// --------------------------

void BSONCollectionCatalogEntry::IndexMetaData::updateTTLSetting(long long newExpireSeconds) {
    BSONObjBuilder b;
    for (BSONObjIterator bi(spec); bi.more();) {
        BSONElement e = bi.next();
        if (e.fieldNameStringData() == "expireAfterSeconds") {
            continue;
        }
        b.append(e);
    }

    b.append("expireAfterSeconds", newExpireSeconds);
    spec = b.obj();
}

// --------------------------

int BSONCollectionCatalogEntry::MetaData::findIndexOffset(StringData name) const {
    for (unsigned i = 0; i < indexes.size(); i++)
        if (indexes[i].name() == name)
            return i;
    return -1;
}

bool BSONCollectionCatalogEntry::MetaData::eraseIndex(StringData name) {
    int indexOffset = findIndexOffset(name);

    if (indexOffset < 0) {
        return false;
    }

    indexes.erase(indexes.begin() + indexOffset);
    return true;
}

void BSONCollectionCatalogEntry::MetaData::rename(StringData toNS) {
    ns = toNS.toString();
    for (size_t i = 0; i < indexes.size(); i++) {
        BSONObj spec = indexes[i].spec;
        BSONObjBuilder b;
        b.append("ns", toNS);
        b.appendElementsUnique(spec);
        indexes[i].spec = b.obj();
    }
}

BSONObj BSONCollectionCatalogEntry::MetaData::toBSON() const {
    BSONObjBuilder b;
    b.append("ns", ns);
    b.append("options", options.toBSON());
    {
        BSONArrayBuilder arr(b.subarrayStart("indexes"));
        for (unsigned i = 0; i < indexes.size(); i++) {
            BSONObjBuilder sub(arr.subobjStart());
            sub.append("spec", indexes[i].spec);
            sub.appendBool("ready", indexes[i].ready);
            sub.appendBool("multikey", indexes[i].multikey);

            if (!indexes[i].multikeyPaths.empty()) {
                BSONObjBuilder subMultikeyPaths(sub.subobjStart("multikeyPaths"));
                appendMultikeyPathsAsBytes(indexes[i].spec.getObjectField("key"),
                                           indexes[i].multikeyPaths,
                                           &subMultikeyPaths);
                subMultikeyPaths.doneFast();
            }

            sub.append("head", static_cast<long long>(indexes[i].head.repr()));
            sub.doneFast();
        }
        arr.doneFast();
    }
    return b.obj();
}

void BSONCollectionCatalogEntry::MetaData::parse(const BSONObj& obj) {
    ns = obj["ns"].valuestrsafe();

    if (obj["options"].isABSONObj()) {
        options.parse(obj["options"].Obj());
    }

    BSONElement indexList = obj["indexes"];

    if (indexList.isABSONObj()) {
        for (BSONElement elt : indexList.Obj()) {
            BSONObj idx = elt.Obj();
            IndexMetaData imd;
            imd.spec = idx["spec"].Obj().getOwned();
            imd.ready = idx["ready"].trueValue();
            if (idx.hasField("head")) {
                imd.head = RecordId(idx["head"].Long());
            } else {
                imd.head = RecordId(idx["head_a"].Int(), idx["head_b"].Int());
            }
            imd.multikey = idx["multikey"].trueValue();

            if (auto multikeyPathsElem = idx["multikeyPaths"]) {
                parseMultikeyPathsFromBytes(multikeyPathsElem.Obj(), &imd.multikeyPaths);
            }

            indexes.push_back(imd);
        }
    }
}
}
