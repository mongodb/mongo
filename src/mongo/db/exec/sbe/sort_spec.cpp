// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/exec/sbe/sort_spec.h"

#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/storage/key_string/key_string.h"

namespace mongo::sbe {
using TypeTags = value::TypeTags;
using Value = value::Value;

key_string::Value SortSpec::generateSortKey(const BSONObj& obj, const CollatorInterface* collator) {
    _sortKeyGen.setCollator(collator);
    return _sortKeyGen.computeSortKeyString(obj);
}

value::SortKeyComponentVector* SortSpec::generateSortKeyComponentVector(
    value::TagValueMaybeOwned obj, const CollatorInterface* collator) {
    // While this function accepts any type of object, for now we simply convert everything
    // to BSON. In the future, we may change this function to avoid the conversion.
    auto bsonObj = [&]() {
        if (obj.tag() == value::TypeTags::bsonObject) {
            auto bsonVal = obj.value();
            if (obj.owned()) {
                _tempVal = std::move(obj);
            }
            return BSONObj{value::bitcastTo<const char*>(bsonVal)};
        } else if (obj.tag() == value::TypeTags::Object) {
            BSONObjBuilder objBuilder;
            bson::convertToBsonObj(objBuilder, value::getObjectView(obj.value()));
            _tempObj = objBuilder.obj();
            return _tempObj;
        } else {
            MONGO_UNREACHABLE_TASSERT(7103703);
        }
    }();

    _sortKeyGen.setCollator(collator);
    // Use the generic API for getting an array of bson elements representing the
    // sort key.
    _sortKeyGen.generateSortKeyComponentVector(bsonObj, &_localBsonEltStorage);

    // Convert this array of BSONElements into the SBE SortKeyComponentVector type.
    {
        size_t i = 0;
        for (auto& elt : _localBsonEltStorage) {
            _localSortKeyComponentStorage.elts[i++] = bson::convertToView(elt);
        }
    }
    return &_localSortKeyComponentStorage;
}

std::pair<TypeTags, Value> SortSpec::compare(TypeTags leftTag,
                                             Value leftVal,
                                             TypeTags rightTag,
                                             Value rightVal,
                                             const CollatorInterface* collator) const {
    if (_sortPattern.size() == 1) {
        auto [cmpTag, cmpVal] = value::compareValue(leftTag, leftVal, rightTag, rightVal, collator);
        if (cmpTag == TypeTags::NumberInt32) {
            auto sign = _sortPattern[0].isAscending ? 1 : -1;
            cmpVal = value::bitcastFrom<int32_t>(value::bitcastTo<int32_t>(cmpVal) * sign);
            return {cmpTag, cmpVal};
        } else {
            return {TypeTags::Nothing, 0};
        }
    }

    if (leftTag != TypeTags::Array || rightTag != TypeTags::Array) {
        return {TypeTags::Nothing, 0};
    }
    auto leftArray = value::getArrayView(leftVal);
    auto rightArray = value::getArrayView(rightVal);
    if (leftArray->size() != _sortPattern.size() || rightArray->size() != _sortPattern.size()) {
        return {TypeTags::Nothing, 0};
    }

    for (size_t i = 0; i < _sortPattern.size(); i++) {
        auto leftElemTagVal = leftArray->getAt(i);
        auto rightElemTagVal = rightArray->getAt(i);
        auto [cmpTag, cmpVal] = value::compareValue(leftElemTagVal.tag,
                                                    leftElemTagVal.value,
                                                    rightElemTagVal.tag,
                                                    rightElemTagVal.value,
                                                    collator);
        if (cmpTag == TypeTags::NumberInt32) {
            if (cmpVal != 0) {
                auto sign = _sortPattern[i].isAscending ? 1 : -1;
                cmpVal = value::bitcastFrom<int32_t>(value::bitcastTo<int32_t>(cmpVal) * sign);
                return {cmpTag, cmpVal};
            }
        } else {
            return {TypeTags::Nothing, 0};
        }
    }

    return {TypeTags::NumberInt32, 0};
}

BtreeKeyGenerator SortSpec::initKeyGen() const {
    tassert(5037003,
            "SortSpec should not be passed an empty sort pattern",
            !_sortPatternBson.isEmpty());

    std::vector<const char*> fields;
    std::vector<BSONElement> fixed;
    for (auto&& elem : _sortPatternBson) {
        fields.push_back(elem.fieldName());

        // BtreeKeyGenerator's constructor's first parameter (the 'fields' vector) and second
        // parameter (the 'fixed' vector) are parallel vectors. The 'fixed' vector allows the
        // caller to specify if the any sort keys have already been determined for one or more
        // of the field paths from the 'fields' vector. In this case, we haven't determined what
        // the sort keys are for any of the fields paths, so we populate the 'fixed' vector with
        // EOO values to indicate this.
        fixed.emplace_back();
    }

    const bool isSetSparseByUser = false;
    auto version = key_string::Version::kLatestVersion;
    auto ordering = Ordering::make(_sortPatternBson);

    return {std::move(fields), std::move(fixed), isSetSparseByUser, version, ordering};
}

size_t SortSpec::getApproximateSize() const {
    auto size = sizeof(SortSpec);
    size += _sortKeyGen.getApproximateSize();
    size += _sortPatternBson.isOwned() ? _sortPatternBson.objsize() : 0;
    return size;
}
}  // namespace mongo::sbe
