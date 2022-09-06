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

#include "mongo/db/storage/bson_collection_catalog_entry.h"

#include <algorithm>
#include <numeric>

#include "mongo/db/field_ref.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/server_options.h"

namespace mongo {

namespace {

// An index will fail to get created if the size in bytes of its key pattern is greater than 2048.
// We use that value to represent the largest number of path components we could ever possibly
// expect to see in an indexed field.
const size_t kMaxKeyPatternPathLength = 2048;

const std::string kTimeseriesBucketsMayHaveMixedSchemaDataFieldName =
    "timeseriesBucketsMayHaveMixedSchemaData";

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
    char multikeyPathsEncodedAsBytes[kMaxKeyPatternPathLength];

    size_t i = 0;
    for (const auto& keyElem : keyPattern) {
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
    for (const auto& elem : multikeyPathsObj) {
        MultikeyComponents multikeyComponents;
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


void BSONCollectionCatalogEntry::IndexMetaData::updateHiddenSetting(bool hidden) {
    // If hidden == false, we remove this field from catalog rather than add a field with false.
    // or else, the old binary can't startup due to the unknown field.
    BSONObjBuilder b;
    for (BSONObjIterator bi(spec); bi.more();) {
        BSONElement e = bi.next();
        if (e.fieldNameStringData() == "hidden") {
            continue;
        }
        b.append(e);
    }

    if (hidden) {
        b.append("hidden", hidden);
    }
    spec = b.obj();
}


void BSONCollectionCatalogEntry::IndexMetaData::updateUniqueSetting(bool unique) {
    // If unique == false, we remove this field from catalog rather than add a field with false.
    BSONObjBuilder b;
    for (BSONObjIterator bi(spec); bi.more();) {
        BSONElement e = bi.next();
        if (e.fieldNameStringData() != "unique") {
            b.append(e);
        }
    }

    if (unique) {
        b.append("unique", unique);
    }
    spec = b.obj();
}

void BSONCollectionCatalogEntry::IndexMetaData::updatePrepareUniqueSetting(bool prepareUnique) {
    // If prepareUnique == false, we remove this field from catalog rather than add a
    // field with false.
    BSONObjBuilder b;
    for (BSONObjIterator bi(spec); bi.more();) {
        BSONElement e = bi.next();
        if (e.fieldNameStringData() != "prepareUnique") {
            b.append(e);
        }
    }

    if (prepareUnique) {
        b.append("prepareUnique", prepareUnique);
    }
    spec = b.obj();
}

// --------------------------

int BSONCollectionCatalogEntry::MetaData::getTotalIndexCount() const {
    return std::count_if(
        indexes.cbegin(), indexes.cend(), [](const auto& index) { return index.isPresent(); });
}

int BSONCollectionCatalogEntry::MetaData::findIndexOffset(StringData name) const {
    for (unsigned i = 0; i < indexes.size(); i++)
        if (indexes[i].nameStringData() == name)
            return i;
    return -1;
}

void BSONCollectionCatalogEntry::MetaData::insertIndex(IndexMetaData indexMetaData) {
    int indexOffset = findIndexOffset(indexMetaData.nameStringData());

    if (indexOffset < 0) {
        indexes.push_back(std::move(indexMetaData));
        return;
    }

    // We have an unused element, was invalidated due to an index drop, that can be reused
    // for this new index.
    indexes[indexOffset] = std::move(indexMetaData);
}

bool BSONCollectionCatalogEntry::MetaData::eraseIndex(StringData name) {
    int indexOffset = findIndexOffset(name);

    if (indexOffset < 0) {
        return false;
    }

    // Zero out the index metadata to be reused later and to keep the indexes of other indexes
    // stable.
    indexes[indexOffset] = {};

    return true;
}

BSONObj BSONCollectionCatalogEntry::MetaData::toBSON(bool hasExclusiveAccess) const {
    BSONObjBuilder b;
    b.append("ns", nss.toStringWithTenantId());
    b.append("options", options.toBSON());
    {
        BSONArrayBuilder arr(b.subarrayStart("indexes"));
        for (unsigned i = 0; i < indexes.size(); i++) {
            if (!indexes[i].isPresent()) {
                continue;
            }

            BSONObjBuilder sub(arr.subobjStart());
            sub.append("spec", indexes[i].spec);
            sub.appendBool("ready", indexes[i].ready);
            {
                stdx::unique_lock lock(indexes[i].multikeyMutex, stdx::defer_lock_t{});
                if (!hasExclusiveAccess) {
                    lock.lock();
                }
                sub.appendBool("multikey", indexes[i].multikey);

                if (!indexes[i].multikeyPaths.empty()) {
                    BSONObjBuilder subMultikeyPaths(sub.subobjStart("multikeyPaths"));
                    appendMultikeyPathsAsBytes(indexes[i].spec.getObjectField("key"),
                                               indexes[i].multikeyPaths,
                                               &subMultikeyPaths);
                    subMultikeyPaths.doneFast();
                }
            }

            sub.append("head", 0ll);  // For backward compatibility with 4.0
            sub.append("backgroundSecondary", indexes[i].isBackgroundSecondaryBuild);

            if (indexes[i].buildUUID) {
                indexes[i].buildUUID->appendToBuilder(&sub, "buildUUID");
            }
            sub.doneFast();
        }
        arr.doneFast();
    }

    if (timeseriesBucketsMayHaveMixedSchemaData) {
        b.append(kTimeseriesBucketsMayHaveMixedSchemaDataFieldName,
                 *timeseriesBucketsMayHaveMixedSchemaData);
    }

    return b.obj();
}

void BSONCollectionCatalogEntry::MetaData::parse(const BSONObj& obj) {
    nss = NamespaceString::parseFromStringExpectTenantIdInMultitenancyMode(
        obj.getStringField("ns").toString());

    if (obj["options"].isABSONObj()) {
        options = uassertStatusOK(
            CollectionOptions::parse(obj["options"].Obj(), CollectionOptions::parseForStorage));
    }

    BSONElement indexList = obj["indexes"];

    if (indexList.isABSONObj()) {
        for (BSONElement elt : indexList.Obj()) {
            BSONObj idx = elt.Obj();
            IndexMetaData imd;
            imd.spec = idx["spec"].Obj().getOwned();
            imd.ready = idx["ready"].trueValue();
            imd.multikey = idx["multikey"].trueValue();

            if (auto multikeyPathsElem = idx["multikeyPaths"]) {
                parseMultikeyPathsFromBytes(multikeyPathsElem.Obj(), &imd.multikeyPaths);
            }

            auto bgSecondary = BSONElement(idx["backgroundSecondary"]);
            // Opt-in to rebuilding behavior for old-format index catalog objects.
            imd.isBackgroundSecondaryBuild = bgSecondary.eoo() || bgSecondary.trueValue();

            if (idx["buildUUID"]) {
                imd.buildUUID = fassert(31353, UUID::parse(idx["buildUUID"]));
            }

            if (!imd.isPresent()) {
                fassertFailedWithStatus(
                    5738500,
                    Status(ErrorCodes::FailedToParse,
                           str::stream() << "invalid index in collection metadata: " << obj));
            }

            indexes.push_back(imd);
        }
    }

    BSONElement timeseriesMixedSchemaElem = obj[kTimeseriesBucketsMayHaveMixedSchemaDataFieldName];
    if (!timeseriesMixedSchemaElem.eoo() && timeseriesMixedSchemaElem.isBoolean()) {
        timeseriesBucketsMayHaveMixedSchemaData = timeseriesMixedSchemaElem.Bool();
    }
}
}  // namespace mongo
