/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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

#include "merizo/platform/basic.h"

#include "merizo/bson/bson_comparator_interface_base.h"

#include <boost/functional/hash.hpp>

#include "merizo/base/simple_string_data_comparator.h"
#include "merizo/bson/bsonelement.h"
#include "merizo/bson/bsonobj.h"
#include "merizo/bson/simple_bsonobj_comparator.h"

namespace merizo {

template <typename T>
void BSONComparatorInterfaceBase<T>::hashCombineBSONObj(
    size_t& seed,
    const BSONObj& objToHash,
    ComparisonRulesSet rules,
    const StringData::ComparatorInterface* stringComparator) {

    if (rules & ComparisonRules::kIgnoreFieldOrder) {
        BSONObjIteratorSorted iter(objToHash);
        while (iter.more()) {
            hashCombineBSONElement(seed, iter.next(), rules, stringComparator);
        }
    } else {
        for (auto elem : objToHash) {
            hashCombineBSONElement(seed, elem, rules, stringComparator);
        }
    }
}

template <typename T>
void BSONComparatorInterfaceBase<T>::hashCombineBSONElement(
    size_t& hash,
    BSONElement elemToHash,
    ComparisonRulesSet rules,
    const StringData::ComparatorInterface* stringComparator) {
    boost::hash_combine(hash, elemToHash.canonicalType());

    const StringData fieldName = elemToHash.fieldNameStringData();
    if ((rules & ComparisonRules::kConsiderFieldName) && !fieldName.empty()) {
        SimpleStringDataComparator::kInstance.hash_combine(hash, fieldName);
    }

    switch (elemToHash.type()) {
        case merizo::EOO:
        case merizo::Undefined:
        case merizo::jstNULL:
        case merizo::MaxKey:
        case merizo::MinKey:
            // These are valueless types
            break;

        case merizo::Bool:
            boost::hash_combine(hash, elemToHash.boolean());
            break;

        case merizo::bsonTimestamp:
            boost::hash_combine(hash, elemToHash.timestamp().asULL());
            break;

        case merizo::Date:
            boost::hash_combine(hash, elemToHash.date().asInt64());
            break;

        case merizo::NumberDecimal: {
            const Decimal128 dcml = elemToHash.numberDecimal();
            if (dcml.toAbs().isGreater(Decimal128(std::numeric_limits<double>::max(),
                                                  Decimal128::kRoundTo34Digits,
                                                  Decimal128::kRoundTowardZero)) &&
                !dcml.isInfinite() && !dcml.isNaN()) {
                // Normalize our decimal to force equivalent decimals
                // in the same cohort to hash to the same value
                Decimal128 dcmlNorm(dcml.normalize());
                boost::hash_combine(hash, dcmlNorm.getValue().low64);
                boost::hash_combine(hash, dcmlNorm.getValue().high64);
                break;
            }
            // Else, fall through and convert the decimal to a double and hash.
            // At this point the decimal fits into the range of doubles, is infinity, or is NaN,
            // which doubles have a cheaper representation for.
        }
        case merizo::NumberDouble:
        case merizo::NumberLong:
        case merizo::NumberInt: {
            // This converts all numbers to doubles, which ignores the low-order bits of
            // NumberLongs > 2**53 and precise decimal numbers without double representations,
            // but that is ok since the hash will still be the same for equal numbers and is
            // still likely to be different for different numbers. (Note: this issue only
            // applies for decimals when they are outside of the valid double range. See
            // the above case.)
            // SERVER-16851
            const double dbl = elemToHash.numberDouble();
            if (std::isnan(dbl)) {
                boost::hash_combine(hash, std::numeric_limits<double>::quiet_NaN());
            } else {
                boost::hash_combine(hash, dbl);
            }
            break;
        }

        case merizo::jstOID:
            elemToHash.__oid().hash_combine(hash);
            break;

        case merizo::String: {
            if (stringComparator) {
                stringComparator->hash_combine(hash, elemToHash.valueStringData());
            } else {
                SimpleStringDataComparator::kInstance.hash_combine(hash,
                                                                   elemToHash.valueStringData());
            }
            break;
        }

        case merizo::Code:
        case merizo::Symbol:
            SimpleStringDataComparator::kInstance.hash_combine(hash, elemToHash.valueStringData());
            break;

        case merizo::Object:
        case merizo::Array:
            hashCombineBSONObj(hash,
                               elemToHash.embeddedObject(),
                               rules | ComparisonRules::kConsiderFieldName,
                               stringComparator);
            break;

        case merizo::DBRef:
        case merizo::BinData:
            // All bytes of the value are required to be identical.
            SimpleStringDataComparator::kInstance.hash_combine(
                hash, StringData(elemToHash.value(), elemToHash.valuesize()));
            break;

        case merizo::RegEx:
            SimpleStringDataComparator::kInstance.hash_combine(hash, elemToHash.regex());
            SimpleStringDataComparator::kInstance.hash_combine(hash, elemToHash.regexFlags());
            break;

        case merizo::CodeWScope: {
            SimpleStringDataComparator::kInstance.hash_combine(
                hash, StringData(elemToHash.codeWScopeCode(), elemToHash.codeWScopeCodeLen()));
            hashCombineBSONObj(hash,
                               elemToHash.codeWScopeObject(),
                               rules | ComparisonRules::kConsiderFieldName,
                               &SimpleStringDataComparator::kInstance);
            break;
        }
    }
}

template class BSONComparatorInterfaceBase<BSONObj>;
template class BSONComparatorInterfaceBase<BSONElement>;

}  // namespace merizo
