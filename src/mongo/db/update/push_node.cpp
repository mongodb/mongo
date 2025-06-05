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

#include "mongo/db/update/push_node.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/mutable_bson/algorithm.h"
#include "mongo/db/exec/mutable_bson/document.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"

#include <cstdlib>
#include <iterator>
#include <limits>
#include <numeric>
#include <set>
#include <string>
#include <type_traits>
#include <utility>

#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

const StringData PushNode::kEachClauseName = "$each"_sd;
const StringData PushNode::kSliceClauseName = "$slice";
const StringData PushNode::kSortClauseName = "$sort";
const StringData PushNode::kPositionClauseName = "$position";

namespace {

/**
 * std::abs(LLONG_MIN) results in undefined behavior on 2's complement systems because the
 * absolute value of LLONG_MIN cannot be represented in a 'long long'.
 *
 * If the input is LLONG_MIN, will return std::abs(LLONG_MIN + 1).
 */
long long safeApproximateAbs(long long val) {
    return val == std::numeric_limits<decltype(val)>::min() ? std::abs(val + 1) : std::abs(val);
}

}  // namespace

Status PushNode::init(BSONElement modExpr, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    invariant(modExpr.ok());

    if (modExpr.type() == BSONType::object && modExpr[kEachClauseName]) {
        const StringDataSet validClauseNames{
            kEachClauseName,
            kSliceClauseName,
            kSortClauseName,
            kPositionClauseName,
        };
        StringDataMap<BSONElement> clausesFound;
        for (auto&& modifier : modExpr.embeddedObject()) {
            auto clauseName = modifier.fieldNameStringData();

            auto foundClauseName = validClauseNames.find(clauseName);
            if (foundClauseName == validClauseNames.end()) {
                return Status(ErrorCodes::BadValue,
                              str::stream() << "Unrecognized clause in $push: "
                                            << modifier.fieldNameStringData());
            }

            auto [insIter, insOk] = clausesFound.insert({*foundClauseName, modifier});
            if (!insOk) {
                return Status(ErrorCodes::BadValue,
                              str::stream() << "Only one " << clauseName << " is supported.");
            }
        }

        // Parse $each.
        auto eachIt = clausesFound.find(kEachClauseName);
        invariant(eachIt != clausesFound.end());  // We already checked for a $each clause.
        const auto& eachClause = eachIt->second;
        if (eachClause.type() != BSONType::array) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "The argument to $each in $push must be"
                                           " an array but it was of type: "
                                        << typeName(eachClause.type()));
        }

        for (auto&& item : eachClause.embeddedObject()) {
            _valuesToPush.push_back(item);
        }

        // Parse (optional) $slice.
        auto sliceIt = clausesFound.find(kSliceClauseName);
        if (sliceIt != clausesFound.end()) {
            auto sliceClause = sliceIt->second;
            auto parsedSliceValue = sliceClause.parseIntegerElementToLong();
            if (parsedSliceValue.isOK()) {
                _slice = parsedSliceValue.getValue();
            } else {
                return Status(ErrorCodes::BadValue,
                              str::stream() << "The value for $slice must "
                                               "be an integer value but was given type: "
                                            << typeName(sliceClause.type()));
            }
        }

        // Parse (optional) $sort.
        auto sortIt = clausesFound.find(kSortClauseName);
        if (sortIt != clausesFound.end()) {
            auto sortClause = sortIt->second;

            if (sortClause.type() == BSONType::object) {
                auto status = pattern_cmp::checkSortClause(sortClause.embeddedObject());

                if (status.isOK()) {
                    _sort = PatternElementCmp(sortClause.embeddedObject(), expCtx->getCollator());
                } else {
                    return status;
                }
            } else if (sortClause.isNumber()) {
                double orderVal = sortClause.Number();
                if (orderVal == -1 || orderVal == 1) {
                    _sort = PatternElementCmp(BSON("" << orderVal), expCtx->getCollator());
                } else {
                    return Status(ErrorCodes::BadValue,
                                  "The $sort element value must be either 1 or -1");
                }
            } else {
                return Status(ErrorCodes::BadValue,
                              "The $sort is invalid: use 1/-1 to sort the whole element, "
                              "or {field:1/-1} to sort embedded fields");
            }
        }

        // Parse (optional) $position.
        auto positionIt = clausesFound.find(kPositionClauseName);
        if (positionIt != clausesFound.end()) {
            auto positionClause = positionIt->second;
            auto parsedPositionValue = positionClause.parseIntegerElementToLong();
            if (parsedPositionValue.isOK()) {
                _position = parsedPositionValue.getValue();
            } else {
                return Status(ErrorCodes::BadValue,
                              str::stream() << "The value for $position must "
                                               "be an integer value, not of type: "
                                            << typeName(positionClause.type()));
            }
        }
    } else {
        // No $each clause. We treat the value of $push as the element to add to the array.
        _valuesToPush.push_back(modExpr);
    }

    return Status::OK();
}

BSONObj PushNode::operatorValue() const {
    BSONObjBuilder bob;
    {
        BSONObjBuilder subBuilder(bob.subobjStart(""));
        {
            // This serialization function always produces $each regardless of whether the input
            // contained it.
            BSONObjBuilder eachBuilder(subBuilder.subarrayStart("$each"));
            for (const auto& value : _valuesToPush)
                eachBuilder << value;
        }
        if (_slice)
            subBuilder << "$slice" << _slice.value();
        if (_position)
            subBuilder << "$position" << _position.value();
        if (_sort) {
            // The sort pattern is stored in a dummy enclosing object that we must unwrap.
            if (_sort->useWholeValue)
                subBuilder << "$sort" << _sort->sortPattern.firstElement();
            else
                subBuilder << "$sort" << _sort->sortPattern;
        }
    }
    return bob.obj();
}

ModifierNode::ModifyResult PushNode::insertElementsWithPosition(
    mutablebson::Element* array,
    boost::optional<long long> position,
    const std::vector<BSONElement>& valuesToPush) {
    if (valuesToPush.empty()) {
        return ModifyResult::kNoOp;
    }

    auto& document = array->getDocument();
    auto firstElementToInsert =
        document.makeElementWithNewFieldName(StringData(), valuesToPush.front());

    // We assume that no array has more than std::numerical_limits<long long>::max() elements.
    long long arraySize = static_cast<long long>(countChildren(*array));

    // We insert the first element of 'valuesToPush' at the location requested in the 'position'
    // variable.
    ModifyResult result;
    if (arraySize == 0) {
        invariant(array->pushBack(firstElementToInsert));
        result = ModifyResult::kNormalUpdate;
    } else if (!position || position.value() > arraySize) {
        invariant(array->pushBack(firstElementToInsert));
        result = ModifyResult::kArrayAppendUpdate;
    } else if (position.value() > 0) {
        auto insertAfter = getNthChild(*array, position.value() - 1);
        invariant(insertAfter.addSiblingRight(firstElementToInsert));
        result = ModifyResult::kNormalUpdate;
    } else if (position.value() < 0 && safeApproximateAbs(position.value()) < arraySize) {
        auto insertAfter =
            getNthChild(*array, arraySize - safeApproximateAbs(position.value()) - 1);
        invariant(insertAfter.addSiblingRight(firstElementToInsert));
        result = ModifyResult::kNormalUpdate;
    } else {
        invariant(array->pushFront(firstElementToInsert));
        result = ModifyResult::kNormalUpdate;
    }

    // We insert all the rest of the elements after the one we just
    // inserted.
    //
    // TODO: The use of std::accumulate here is maybe questionable
    // given that we are ignoring the return value. MSVC flagged this,
    // and we worked around by tagging the result as unused.
    [[maybe_unused]] auto ignored =
        std::accumulate(std::next(valuesToPush.begin()),
                        valuesToPush.end(),
                        firstElementToInsert,
                        [&document](auto&& insertAfter, auto& valueToInsert) {
                            auto nextElementToInsert =
                                document.makeElementWithNewFieldName(StringData(), valueToInsert);
                            invariant(insertAfter.addSiblingRight(nextElementToInsert));
                            return nextElementToInsert;
                        });

    return result;
}

ModifierNode::ModifyResult PushNode::performPush(mutablebson::Element* element,
                                                 const FieldRef* elementPath) const {
    if (element->getType() != BSONType::array) {
        invariant(elementPath);  // We can only hit this error if we are updating an existing path.
        auto idElem = mutablebson::findFirstChildNamed(element->getDocument().root(), "_id");
        uasserted(ErrorCodes::BadValue,
                  str::stream() << "The field '" << elementPath->dottedField() << "'"
                                << " must be an array but is of type "
                                << typeName(element->getType()) << " in document {"
                                << (idElem.ok() ? idElem.toString() : "no id") << "}");
    }

    auto result = insertElementsWithPosition(element, _position, _valuesToPush);

    if (_sort) {
        result = ModifyResult::kNormalUpdate;
        sortChildren(*element, *_sort);
    }

    if (_slice) {
        const auto sliceAbs = safeApproximateAbs(_slice.value());

        while (static_cast<long long>(countChildren(*element)) > sliceAbs) {
            result = ModifyResult::kNormalUpdate;
            if (_slice.value() >= 0) {
                invariant(element->popBack());
            } else {
                // A negative value in '_slice' trims the array down to abs(_slice) but removes
                // entries from the front of the array instead of the back.
                invariant(element->popFront());
            }
        }
    }

    return result;
}

ModifierNode::ModifyResult PushNode::updateExistingElement(mutablebson::Element* element,
                                                           const FieldRef& elementPath) const {
    return performPush(element, &elementPath);
}

void PushNode::logUpdate(LogBuilderInterface* logBuilder,
                         const RuntimeUpdatePath& pathTaken,
                         mutablebson::Element element,
                         ModifyResult modifyResult,
                         boost::optional<int> createdFieldIdx) const {
    invariant(logBuilder);

    if (modifyResult.type == ModifyResult::kNormalUpdate) {
        uassertStatusOK(logBuilder->logUpdatedField(pathTaken, element));
    } else if (modifyResult.type == ModifyResult::kCreated) {
        invariant(createdFieldIdx);
        uassertStatusOK(logBuilder->logCreatedField(pathTaken, *createdFieldIdx, element));
    } else if (modifyResult.type == ModifyResult::kArrayAppendUpdate) {
        // This update only modified the array by appending entries to the end. Rather than writing
        // out the entire contents of the array, we create oplog entries for the newly appended
        // elements.
        const auto numAppended = _valuesToPush.size();
        const auto arraySize = countChildren(element);

        // We have to copy the field ref provided in order to use RuntimeUpdatePathTempAppend.
        RuntimeUpdatePath pathTakenCopy = pathTaken;
        invariant(arraySize > numAppended);
        auto position = arraySize - numAppended;
        for (const auto& valueToLog : _valuesToPush) {
            const std::string positionAsString = std::to_string(position);

            RuntimeUpdatePathTempAppend tempAppend(
                pathTakenCopy, positionAsString, RuntimeUpdatePath::ComponentType::kArrayIndex);
            uassertStatusOK(
                logBuilder->logCreatedField(pathTakenCopy, pathTakenCopy.size() - 1, valueToLog));

            ++position;
        }
    } else {
        MONGO_UNREACHABLE;
    }
}

void PushNode::setValueForNewElement(mutablebson::Element* element) const {
    BSONObj emptyArray;
    invariant(element->setValueArray(emptyArray));
    (void)performPush(element, nullptr);
}

}  // namespace mongo
