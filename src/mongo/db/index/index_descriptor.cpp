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

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/index/index_descriptor.h"

#include <algorithm>

#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/util/log.h"

namespace mongo {

using IndexVersion = IndexDescriptor::IndexVersion;

namespace {
void populateOptionsMap(std::map<StringData, BSONElement>& theMap, const BSONObj& spec) {
    BSONObjIterator it(spec);
    while (it.more()) {
        const BSONElement e = it.next();

        StringData fieldName = e.fieldNameStringData();
        if (fieldName == IndexDescriptor::kKeyPatternFieldName ||
            fieldName == IndexDescriptor::kNamespaceFieldName ||  // removed in 4.4
            fieldName == IndexDescriptor::kIndexNameFieldName ||
            fieldName ==
                IndexDescriptor::kIndexVersionFieldName ||  // not considered for equivalence
            fieldName == IndexDescriptor::kTextVersionFieldName ||      // same as index version
            fieldName == IndexDescriptor::k2dsphereVersionFieldName ||  // same as index version
            fieldName ==
                IndexDescriptor::kBackgroundFieldName ||  // this is a creation time option only
            fieldName == IndexDescriptor::kDropDuplicatesFieldName ||  // this is now ignored
            fieldName == IndexDescriptor::kSparseFieldName ||          // checked specially
            fieldName == IndexDescriptor::kUniqueFieldName             // check specially
        ) {
            continue;
        }
        theMap[fieldName] = e;
    }
}
}  // namespace

constexpr StringData IndexDescriptor::k2dIndexBitsFieldName;
constexpr StringData IndexDescriptor::k2dIndexMaxFieldName;
constexpr StringData IndexDescriptor::k2dIndexMinFieldName;
constexpr StringData IndexDescriptor::k2dsphereCoarsestIndexedLevel;
constexpr StringData IndexDescriptor::k2dsphereFinestIndexedLevel;
constexpr StringData IndexDescriptor::k2dsphereVersionFieldName;
constexpr StringData IndexDescriptor::kBackgroundFieldName;
constexpr StringData IndexDescriptor::kCollationFieldName;
constexpr StringData IndexDescriptor::kDefaultLanguageFieldName;
constexpr StringData IndexDescriptor::kDropDuplicatesFieldName;
constexpr StringData IndexDescriptor::kExpireAfterSecondsFieldName;
constexpr StringData IndexDescriptor::kGeoHaystackBucketSize;
constexpr StringData IndexDescriptor::kIndexNameFieldName;
constexpr StringData IndexDescriptor::kIndexVersionFieldName;
constexpr StringData IndexDescriptor::kKeyPatternFieldName;
constexpr StringData IndexDescriptor::kLanguageOverrideFieldName;
constexpr StringData IndexDescriptor::kNamespaceFieldName;
constexpr StringData IndexDescriptor::kPartialFilterExprFieldName;
constexpr StringData IndexDescriptor::kPathProjectionFieldName;
constexpr StringData IndexDescriptor::kSparseFieldName;
constexpr StringData IndexDescriptor::kStorageEngineFieldName;
constexpr StringData IndexDescriptor::kTextVersionFieldName;
constexpr StringData IndexDescriptor::kUniqueFieldName;
constexpr StringData IndexDescriptor::kWeightsFieldName;

IndexDescriptor::IndexDescriptor(Collection* collection,
                                 const std::string& accessMethodName,
                                 BSONObj infoObj)
    : _collection(collection),
      _accessMethodName(accessMethodName),
      _indexType(IndexNames::nameToType(accessMethodName)),
      _infoObj(infoObj.getOwned()),
      _numFields(infoObj.getObjectField(IndexDescriptor::kKeyPatternFieldName).nFields()),
      _keyPattern(infoObj.getObjectField(IndexDescriptor::kKeyPatternFieldName).getOwned()),
      _projection(infoObj.getObjectField(IndexDescriptor::kPathProjectionFieldName).getOwned()),
      _indexName(infoObj.getStringField(IndexDescriptor::kIndexNameFieldName)),
      _isIdIndex(isIdIndexPattern(_keyPattern)),
      _sparse(infoObj[IndexDescriptor::kSparseFieldName].trueValue()),
      _unique(_isIdIndex || infoObj[kUniqueFieldName].trueValue()),
      _partial(!infoObj[kPartialFilterExprFieldName].eoo()),
      _cachedEntry(nullptr) {
    BSONElement e = _infoObj[IndexDescriptor::kIndexVersionFieldName];
    fassert(50942, e.isNumber());
    _version = static_cast<IndexVersion>(e.numberInt());

    if (BSONElement filterElement = _infoObj[kPartialFilterExprFieldName]) {
        invariant(filterElement.isABSONObj());
        _partialFilterExpression = filterElement.Obj().getOwned();
    }

    if (BSONElement collationElement = _infoObj[kCollationFieldName]) {
        invariant(collationElement.isABSONObj());
        _collation = collationElement.Obj().getOwned();
    }
}

bool IndexDescriptor::isIndexVersionSupported(IndexVersion indexVersion) {
    switch (indexVersion) {
        case IndexVersion::kV1:
        case IndexVersion::kV2:
            return true;
    }
    return false;
}

std::set<IndexVersion> IndexDescriptor::getSupportedIndexVersions() {
    return {IndexVersion::kV1, IndexVersion::kV2};
}

Status IndexDescriptor::isIndexVersionAllowedForCreation(
    IndexVersion indexVersion,
    const ServerGlobalParams::FeatureCompatibility& featureCompatibility,
    const BSONObj& indexSpec) {
    switch (indexVersion) {
        case IndexVersion::kV1:
        case IndexVersion::kV2:
            return Status::OK();
    }
    return {ErrorCodes::CannotCreateIndex,
            str::stream() << "Invalid index specification " << indexSpec
                          << "; cannot create an index with v=" << static_cast<int>(indexVersion)};
}

IndexVersion IndexDescriptor::getDefaultIndexVersion() {
    return IndexVersion::kV2;
}

bool IndexDescriptor::isMultikey() const {
    return _collection->getIndexCatalog()->isMultikey(this);
}

MultikeyPaths IndexDescriptor::getMultikeyPaths(OperationContext* opCtx) const {
    return _collection->getIndexCatalog()->getMultikeyPaths(opCtx, this);
}

const IndexCatalog* IndexDescriptor::getIndexCatalog() const {
    return _collection->getIndexCatalog();
}

const NamespaceString& IndexDescriptor::parentNS() const {
    return _collection->ns();
}

bool IndexDescriptor::areIndexOptionsEquivalent(const IndexDescriptor* other) const {
    if (isSparse() != other->isSparse()) {
        return false;
    }

    if (!isIdIndex() && unique() != other->unique()) {
        // Note: { _id: 1 } or { _id: -1 } implies unique: true.
        return false;
    }

    // Then compare the rest of the options.

    std::map<StringData, BSONElement> existingOptionsMap;
    populateOptionsMap(existingOptionsMap, infoObj());

    std::map<StringData, BSONElement> newOptionsMap;
    populateOptionsMap(newOptionsMap, other->infoObj());

    return existingOptionsMap.size() == newOptionsMap.size() &&
        std::equal(existingOptionsMap.begin(),
                   existingOptionsMap.end(),
                   newOptionsMap.begin(),
                   [](const std::pair<StringData, BSONElement>& lhs,
                      const std::pair<StringData, BSONElement>& rhs) {
                       return lhs.first == rhs.first &&
                           SimpleBSONElementComparator::kInstance.evaluate(lhs.second ==
                                                                           rhs.second);
                   });
}

}  // namespace mongo
