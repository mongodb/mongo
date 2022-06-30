/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/cst/c_node.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/util/hex.h"
#include "mongo/util/overloaded_visitor.h"

#include <iterator>
#include <limits>
#include <numeric>
#include <type_traits>

namespace mongo {
using namespace std::string_literals;
namespace {
auto tabs(int num) {
    std::string out;
    for (; num > 0; num--)
        out += "\t";
    return out;
}

auto printFieldname(const CNode::Fieldname& fieldname) {
    return stdx::visit(
        OverloadedVisitor{
            [](const KeyFieldname& key) { return "<KeyFieldname "s + toStringData(key) + ">"; },
            [](const UserFieldname& user) { return "<UserFieldname "s + user + ">"; },
            [](const FieldnamePath& path) {
                return stdx::visit(
                    OverloadedVisitor{
                        [&](const ProjectionPath& projPath) {
                            return "<ProjectionPath "s + path::vectorToString(projPath) + ">";
                        },
                        [&](const PositionalProjectionPath& posProjPath) {
                            return "<PositionalProjectionPath "s +
                                path::vectorToString(posProjPath) + ">";
                        },
                        [&](const SortPath& sortPath) {
                            return "<SortPath "s + path::vectorToString(sortPath) + ">";
                        }},
                    path);
            }},
        fieldname);
}

auto printNonZeroKey(const NonZeroKey& nonZeroKey) {
    return stdx::visit(
        OverloadedVisitor{
            [](const int& keyInt) { return "int "s + std::to_string(keyInt); },
            [](const long long& keyLong) { return "long "s + std::to_string(keyLong); },
            [](const double& keyDouble) { return "double "s + std::to_string(keyDouble); },
            [](const Decimal128& keyDecimal) { return "decimal "s + keyDecimal.toString(); }},
        nonZeroKey);
}

template <typename T>
auto printValue(const T& payload) {
    return stdx::visit(
        OverloadedVisitor{
            [](const CNode::ArrayChildren&) { return "<Array>"s; },
            [](const CNode::ObjectChildren&) { return "<Object>"s; },
            [](const CompoundInclusionKey&) { return "<CompoundInclusionKey>"s; },
            [](const CompoundExclusionKey&) { return "<CompoundExclusionKey>"s; },
            [](const CompoundInconsistentKey&) { return "<CompoundInconsistentKey>"s; },
            [](const KeyValue& value) { return "<KeyValue "s + toStringData(value) + ">"; },
            [](const NonZeroKey& nonZeroKey) {
                return "<NonZeroKey of type "s + printNonZeroKey(nonZeroKey) + ">";
            },
            [](const ValuePath& valuePath) {
                return stdx::visit(
                    OverloadedVisitor{[&](const AggregationPath& aggPath) {
                                          return "<AggregationPath "s +
                                              path::vectorToString(aggPath) + ">";
                                      },
                                      [&](const AggregationVariablePath& aggVarPath) {
                                          return "<AggregationVariablePath "s +
                                              path::vectorToString(aggVarPath) + ">";
                                      }},
                    valuePath);
            },
            [](const UserDouble& userDouble) {
                return "<UserDouble "s + std::to_string(userDouble) + ">";
            },
            [](const UserString& userString) { return "<UserString "s + userString + ">"; },
            [](const UserBinary& userBinary) {
                return "<UserBinary "s + typeName(userBinary.type) + ", " +
                    hexblob::encode(userBinary.data, std::max(userBinary.length, 0)) + ">";
            },
            [](const UserUndefined& userUndefined) { return "<UserUndefined>"s; },
            [](const UserObjectId& userObjectId) {
                return "<UserObjectId "s + userObjectId.toString() + ">";
            },
            [](const UserBoolean& userBoolean) {
                return "<UserBoolean "s + (userBoolean ? "true" : "false") + ">";
            },
            [](const UserDate& userDate) {
                return "<UserDate "s +
                    [&] {
                        if (auto string = TimeZoneDatabase::utcZone().formatDate(
                                "%Y-%m-%dT%H:%M:%S.%LZ", userDate);
                            string.isOK())
                            return string.getValue();
                        else
                            return "illegal date"s;
                    }() +
                    ">";
            },
            [](const UserNull& userNull) { return "<UserNull>"s; },
            [](const UserRegex& userRegex) {
                return "<UserRegex "s + "/" + userRegex.pattern + "/" + userRegex.flags + ">";
            },
            [](const UserDBPointer& userDBPointer) {
                return "<UserDBPointer "s + userDBPointer.ns + ", " + userDBPointer.oid.toString() +
                    ">";
            },
            [](const UserJavascript& userJavascript) {
                return "<UserJavascript "s + userJavascript.code + ">";
            },
            [](const UserSymbol& userSymbol) { return "<UserSymbol "s + userSymbol.symbol + ">"; },
            [](const UserJavascriptWithScope& userJavascriptWithScope) {
                return "<UserJavascriptWithScope "s + userJavascriptWithScope.code + ", " +
                    userJavascriptWithScope.scope.toString() + ">";
            },
            [](const UserInt& userInt) { return "<UserInt "s + std::to_string(userInt) + ">"; },
            [](const UserTimestamp& userTimestamp) {
                return "<UserTimestamp "s + userTimestamp.toString() + ">";
            },
            [](const UserLong& userLong) { return "<UserLong "s + std::to_string(userLong) + ">"; },
            [](const UserDecimal& userDecimal) {
                return "<UserDecimal "s + userDecimal.toString() + ">";
            },
            [](const UserMinKey& userMinKey) { return "<UserMinKey>"s; },
            [](const UserMaxKey& userMaxKey) { return "<UserMaxKey>"s; }},
        payload);
}

}  // namespace

std::string CNode::toStringHelper(int numTabs) const {
    return stdx::visit(
        OverloadedVisitor{
            [numTabs](const ArrayChildren& children) {
                return std::accumulate(children.cbegin(),
                                       children.cend(),
                                       tabs(numTabs) + "[\n",
                                       [numTabs](auto&& string, auto&& child) {
                                           return string + child.toStringHelper(numTabs + 1) + "\n";
                                       }) +
                    tabs(numTabs) + "]";
            },
            [numTabs](const ObjectChildren& children) {
                return std::accumulate(children.cbegin(),
                                       children.cend(),
                                       tabs(numTabs) + "{\n",
                                       [numTabs](auto&& string, auto&& childpair) {
                                           return string + tabs(numTabs) +
                                               printFieldname(childpair.first) + " :\n" +
                                               childpair.second.toStringHelper(numTabs + 1) + "\n";
                                       }) +
                    tabs(numTabs) + "}";
            },
            [numTabs](const CompoundInclusionKey& compoundKey) {
                return tabs(numTabs) + "<CompoundInclusionKey>\n" +
                    compoundKey.obj->toStringHelper(numTabs + 1);
            },
            [numTabs](const CompoundExclusionKey& compoundKey) {
                return tabs(numTabs) + "<CompoundExclusionKey>\n" +
                    compoundKey.obj->toStringHelper(numTabs + 1);
            },
            [numTabs](const CompoundInconsistentKey& compoundKey) {
                return tabs(numTabs) + "<CompoundInconsistentKey>\n" +
                    compoundKey.obj->toStringHelper(numTabs + 1);
            },
            [this, numTabs](auto&&) { return tabs(numTabs) + printValue(payload); }},
        payload);
}

std::pair<BSONObj, bool> CNode::toBsonWithArrayIndicator() const {
    auto addChild = [](auto&& bson, auto&& fieldname, auto&& child) {
        // This is a non-compound field. pull the BSONElement out of it's BSONObj shell and add it.
        if (auto [childBson, isArray] = child.toBsonWithArrayIndicator();
            !childBson.isEmpty() && childBson.firstElementFieldNameStringData().empty())
            return bson.addField(
                childBson
                    .replaceFieldNames(BSON(std::forward<decltype(fieldname)>(fieldname) << ""))
                    .firstElement());
        // This field is an array. Reconstruct with BSONArray and add it.
        else if (isArray)
            return bson.addField(
                BSON(std::forward<decltype(fieldname)>(fieldname) << BSONArray{childBson})
                    .firstElement());
        // This field is an object. Add it directly.
        else
            return bson.addField(
                BSON(std::forward<decltype(fieldname)>(fieldname) << childBson).firstElement());
    };

    return stdx::visit(
        OverloadedVisitor{
            // Build an array which will lose its identity and appear as a BSONObj
            [&](const ArrayChildren& children) {
                return std::pair{
                    std::accumulate(children.cbegin(),
                                    children.cend(),
                                    BSONObj{},
                                    [&, fieldCount = 0u](auto&& bson, auto&& child) mutable {
                                        return addChild(std::forward<decltype(bson)>(bson),
                                                        std::to_string(fieldCount++),
                                                        std::forward<decltype(child)>(child));
                                    }),
                    true};
            },
            // Build an object in a BSONObj.
            [&](const ObjectChildren& children) {
                return std::pair{std::accumulate(children.cbegin(),
                                                 children.cend(),
                                                 BSONObj{},
                                                 [&](auto&& bson, auto&& childPair) {
                                                     return addChild(
                                                         std::forward<decltype(bson)>(bson),
                                                         printFieldname(childPair.first),
                                                         childPair.second);
                                                 }),
                                 false};
            },
            // Build a compound inclusion key wrapper in a BSONObj.
            [&](const CompoundInclusionKey& compoundKey) {
                return std::pair{addChild(BSONObj{}, "<CompoundInclusionKey>"s, *compoundKey.obj),
                                 false};
            },
            // Build a compound exclusion key wrapper in a BSONObj.
            [&](const CompoundExclusionKey& compoundKey) {
                return std::pair{addChild(BSONObj{}, "<CompoundExclusionKey>"s, *compoundKey.obj),
                                 false};
            },
            // Build a compound exclusion key wrapper in a BSONObj.
            [&](const CompoundInconsistentKey& compoundKey) {
                return std::pair{
                    addChild(BSONObj{}, "<CompoundInconsistentKey>"s, *compoundKey.obj), false};
            },
            // Build a non-compound field in a BSONObj shell.
            [this](auto&&) {
                return std::pair{BSON("" << printValue(payload)), false};
            }},
        payload);
}

bool CNode::isNumber() const {
    return stdx::visit(
        OverloadedVisitor{
            [](const UserLong&) { return true; },
            [](const UserDouble&) { return true; },
            [](const UserDecimal&) { return true; },
            [](const UserInt&) { return true; },
            [](auto&&) { return false; },
        },
        payload);
}

int CNode::numberInt() const {
    // BSONElement has no safeNumberInt, so use CNode::numberLong which uses
    // BSONElement::safeNumberLong. We don't want to use BSONElement::numberInt because it has
    // undefined behavior for certain inputs (for one example, NaN).

    // safeNumberLong returns the LLONG_MIN/LLONG_MAX when the original value is too big/small
    // for a long long, so imitate that behavior here for int.
    long long val = numberLong();
    constexpr int max = std::numeric_limits<int>::max();
    constexpr int min = std::numeric_limits<int>::min();
    if (val > static_cast<long long>(max))
        return max;
    if (val < static_cast<long long>(min))
        return min;
    return static_cast<long long>(val);
}

long long CNode::numberLong() const {
    return stdx::visit(
        OverloadedVisitor{[](const UserDouble& userDouble) {
                              return (BSON("" << userDouble).firstElement()).safeNumberLong();
                          },
                          [](const UserInt& userInt) {
                              return (BSON("" << userInt).firstElement()).safeNumberLong();
                          },
                          [](const UserLong& userLong) { return userLong; },
                          [](const UserDecimal& userDecimal) {
                              return (BSON("" << userDecimal).firstElement()).safeNumberLong();
                          },
                          [](auto &&) -> UserLong { MONGO_UNREACHABLE }},
        payload);
}

}  // namespace mongo
