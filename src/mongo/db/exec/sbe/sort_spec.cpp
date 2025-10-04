/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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


#include "mongo/db/exec/sbe/sort_spec.h"

#include "mongo/base/compare_numbers.h"
#include "mongo/db/exec/js_function.h"
#include "mongo/db/exec/sbe/makeobj_spec.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/values/value_builder.h"
#include "mongo/db/exec/sbe/values/value_printer.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/pcre_util.h"

namespace mongo::sbe {
using TypeTags = value::TypeTags;
using Value = value::Value;

key_string::Value SortSpec::generateSortKey(const BSONObj& obj, const CollatorInterface* collator) {
    _sortKeyGen.setCollator(collator);
    return _sortKeyGen.computeSortKeyString(obj);
}

value::SortKeyComponentVector* SortSpec::generateSortKeyComponentVector(
    FastTuple<bool, value::TypeTags, value::Value> obj, const CollatorInterface* collator) {
    auto [objOwned, objTag, objVal] = obj;
    ValueGuard guard(objOwned, objTag, objVal);

    // While this function accepts any type of object, for now we simply convert everything
    // to BSON. In the future, we may change this function to avoid the conversion.
    auto bsonObj = [&, objTag = objTag, objVal = objVal, objOwned = objOwned]() {
        if (objTag == value::TypeTags::bsonObject) {
            if (objOwned) {
                // Take ownership of the temporary object here.
                _tempVal.emplace(objTag, objVal);
                guard.reset();
            }
            return BSONObj{value::bitcastTo<const char*>(objVal)};
        } else if (objTag == value::TypeTags::Object) {
            BSONObjBuilder objBuilder;
            bson::convertToBsonObj(objBuilder, value::getObjectView(objVal));
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
            _localSortKeyComponentStorage.elts[i++] = bson::convertFrom<true>(elt);
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
        auto [leftElemTag, leftElemVal] = leftArray->getAt(i);
        auto [rightElemTag, rightElemVal] = rightArray->getAt(i);
        auto [cmpTag, cmpVal] =
            value::compareValue(leftElemTag, leftElemVal, rightElemTag, rightElemVal, collator);
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

    const bool isSparse = false;
    auto version = key_string::Version::kLatestVersion;
    auto ordering = Ordering::make(_sortPatternBson);

    return {std::move(fields), std::move(fixed), isSparse, version, ordering};
}

size_t SortSpec::getApproximateSize() const {
    auto size = sizeof(SortSpec);
    size += _sortKeyGen.getApproximateSize();
    size += _sortPatternBson.isOwned() ? _sortPatternBson.objsize() : 0;
    return size;
}
}  // namespace mongo::sbe
