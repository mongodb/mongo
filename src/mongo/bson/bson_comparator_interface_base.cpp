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

#include <boost/container_hash/extensions.hpp>
#include <cmath>
#include <limits>

#include "mongo/base/string_data_comparator.h"
#include "mongo/bson/bson_comparator_interface_base.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/platform/decimal128.h"
#include "mongo/util/time_support.h"

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

    const StringData fieldName = elemToHash.fieldNameStringData();
    if ((rules & ComparisonRules::kConsiderFieldName) && !fieldName.empty()) {
        simpleStringDataComparator.hash_combine(hash, fieldName);
    }

    switch (elemToHash.type()) {
        case mongo::EOO:
        case mongo::Undefined:
        case mongo::jstNULL:
        case mongo::MaxKey:
        case mongo::MinKey:
            // These are valueless types
            break;

        case mongo::Bool:
            boost::hash_combine(hash, elemToHash.boolean());
            break;

        case mongo::bsonTimestamp:
            boost::hash_combine(hash, elemToHash.timestamp().asULL());
            break;

        case mongo::Date:
            boost::hash_combine(hash, elemToHash.date().asInt64());
            break;

        case mongo::NumberDecimal: {
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
        case mongo::NumberDouble:
        case mongo::NumberLong:
        case mongo::NumberInt: {
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

        case mongo::jstOID:
            elemToHash.__oid().hash_combine(hash);
            break;

        case mongo::String: {
            if (stringComparator) {
                stringComparator->hash_combine(hash, elemToHash.valueStringData());
            } else {
                simpleStringDataComparator.hash_combine(hash, elemToHash.valueStringData());
            }
            break;
        }

        case mongo::Code:
        case mongo::Symbol:
            simpleStringDataComparator.hash_combine(hash, elemToHash.valueStringData());
            break;

        case mongo::Object:
        case mongo::Array:
            hashCombineBSONObj(hash,
                               elemToHash.embeddedObject(),
                               rules | ComparisonRules::kConsiderFieldName,
                               stringComparator);
            break;

        case mongo::DBRef:
        case mongo::BinData:
            // All bytes of the value are required to be identical.
            simpleStringDataComparator.hash_combine(
                hash, StringData(elemToHash.value(), elemToHash.valuesize()));
            break;

        case mongo::RegEx:
            simpleStringDataComparator.hash_combine(hash, elemToHash.regex());
            simpleStringDataComparator.hash_combine(hash, elemToHash.regexFlags());
            break;

        case mongo::CodeWScope: {
            simpleStringDataComparator.hash_combine(
                hash, StringData(elemToHash.codeWScopeCode(), elemToHash.codeWScopeCodeLen()));
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
