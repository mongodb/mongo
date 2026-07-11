// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bson_comparator_interface_base.h"

#include "mongo/base/string_data_comparator.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/platform/decimal128.h"
#include "mongo/util/time_support.h"

#include <cmath>
#include <limits>
#include <string_view>

#include <boost/functional/hash.hpp>

namespace mongo {

template <typename T>
void BSONComparatorInterfaceBase<T>::hashCombineBSONObj(
    size_t& seed,
    const BSONObj& objToHash,
    ComparisonRulesSet rules,
    const StringDataComparator* stringComparator) {

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
    const StringDataComparator* stringComparator) {
    boost::hash_combine(hash, elemToHash.canonicalType());

    const std::string_view fieldName = elemToHash.fieldNameStringData();
    if ((rules & ComparisonRules::kConsiderFieldName) && !fieldName.empty()) {
        simpleStringDataComparator.hash_combine(hash, fieldName);
    }

    switch (elemToHash.type()) {
        case mongo::BSONType::eoo:
        case mongo::BSONType::undefined:
        case mongo::BSONType::null:
        case mongo::BSONType::maxKey:
        case mongo::BSONType::minKey:
            // These are valueless types
            break;

        case mongo::BSONType::boolean:
            boost::hash_combine(hash, elemToHash.boolean());
            break;

        case mongo::BSONType::timestamp:
            boost::hash_combine(hash, elemToHash.timestamp().asULL());
            break;

        case mongo::BSONType::date:
            boost::hash_combine(hash, elemToHash.date().asInt64());
            break;

        case mongo::BSONType::numberDecimal: {
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
            [[fallthrough]];
        }
        case mongo::BSONType::numberDouble:
        case mongo::BSONType::numberLong:
        case mongo::BSONType::numberInt: {
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

        case mongo::BSONType::oid:
            elemToHash.__oid().hash_combine(hash);
            break;

        case mongo::BSONType::string: {
            if (stringComparator) {
                stringComparator->hash_combine(hash, elemToHash.valueStringData());
            } else {
                simpleStringDataComparator.hash_combine(hash, elemToHash.valueStringData());
            }
            break;
        }

        case mongo::BSONType::code:
        case mongo::BSONType::symbol:
            simpleStringDataComparator.hash_combine(hash, elemToHash.valueStringData());
            break;

        case mongo::BSONType::object:
        case mongo::BSONType::array:
            hashCombineBSONObj(hash,
                               elemToHash.embeddedObject(),
                               rules | ComparisonRules::kConsiderFieldName,
                               stringComparator);
            break;

        case mongo::BSONType::dbRef:
        case mongo::BSONType::binData:
            // All bytes of the value are required to be identical.
            simpleStringDataComparator.hash_combine(
                hash, std::string_view(elemToHash.value(), elemToHash.valuesize()));
            break;

        case mongo::BSONType::regEx:
            simpleStringDataComparator.hash_combine(hash, elemToHash.regex());
            simpleStringDataComparator.hash_combine(hash, elemToHash.regexFlags());
            break;

        case mongo::BSONType::codeWScope: {
            simpleStringDataComparator.hash_combine(
                hash,
                std::string_view(elemToHash.codeWScopeCode(), elemToHash.codeWScopeCodeLen()));
            hashCombineBSONObj(hash,
                               elemToHash.codeWScopeObject(),
                               rules | ComparisonRules::kConsiderFieldName,
                               &simpleStringDataComparator);
            break;
        }
    }
}

template class BSONComparatorInterfaceBase<BSONObj>;
template class BSONComparatorInterfaceBase<BSONElement>;

}  // namespace mongo
