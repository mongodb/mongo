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

#include <algorithm>
#include <iterator>

#include "mongo/db/exec/sbe/makeobj_spec.h"

#include "mongo/db/exec/sbe/size_estimator.h"

namespace mongo::sbe {
StringListSet MakeObjSpec::buildFieldDict(std::vector<std::string> names) {
    if (actions.empty()) {
        numKeepOrDrops = names.size();
        numValueArgs = 0;
        numMandatoryLambdas = 0;
        numMandatoryMakeObjs = 0;
        totalNumArgs = 0;

        actions = std::vector<FieldAction>{};
        actions.resize(numKeepOrDrops);

        return StringListSet(std::move(names));
    }

    tassert(7103500,
            "Expected 'names' and 'fieldsInfos' to be the same size",
            names.size() == actions.size());

    std::vector<std::string> keepOrDrops;

    numKeepOrDrops = 0;
    numValueArgs = 0;
    numMandatoryLambdas = 0;
    numMandatoryMakeObjs = 0;
    totalNumArgs = 0;

    size_t endPos = 0;

    for (size_t i = 0; i < names.size(); ++i) {
        if (actions[i].isKeepOrDrop()) {
            ++numKeepOrDrops;
            keepOrDrops.emplace_back(std::move(names[i]));
        } else {
            if (actions[i].isValueArg()) {
                ++numValueArgs;
            } else if (actions[i].isLambdaArg() &&
                       !actions[i].getLambdaArg().returnsNothingOnMissingInput) {
                ++numMandatoryLambdas;
            } else if (actions[i].isMakeObj() &&
                       !actions[i].getMakeObjSpec()->returnsNothingOnMissingInput()) {
                ++numMandatoryMakeObjs;
            }

            if (actions[i].isValueArg() || actions[i].isLambdaArg()) {
                ++totalNumArgs;
            } else if (actions[i].isMakeObj()) {
                totalNumArgs += actions[i].getMakeObjSpec()->totalNumArgs;
            }

            if (i != endPos) {
                names[endPos] = std::move(names[i]);
                actions[endPos] = std::move(actions[i]);
            }
            ++endPos;
        }
    }

    if (endPos != names.size()) {
        names.erase(names.begin() + endPos, names.end());
        actions.erase(actions.begin() + endPos, actions.end());
    }

    std::vector<FieldAction> newActions;
    newActions.resize(numKeepOrDrops);

    std::move(actions.begin(), actions.end(), std::back_inserter(newActions));
    actions = std::move(newActions);

    std::vector<std::string> newNames = std::move(keepOrDrops);
    std::move(names.begin(), names.end(), std::back_inserter(newNames));

    return StringListSet(std::move(newNames));
}

size_t MakeObjSpec::getApproximateSize() const {
    auto size = sizeof(MakeObjSpec);

    size += size_estimator::estimate(fields);

    size += size_estimator::estimateContainerOnly(actions);

    for (size_t i = 0; i < actions.size(); ++i) {
        if (actions[i].isMakeObj()) {
            size += actions[i].getMakeObjSpec()->getApproximateSize();
        }
    }

    return size;
}

MakeObjSpec::FieldAction MakeObjSpec::FieldAction::clone() const {
    return stdx::visit(OverloadedVisitor{[](KeepOrDrop kd) -> FieldAction { return kd; },
                                         [](ValueArg va) -> FieldAction { return va; },
                                         [](LambdaArg la) -> FieldAction { return la; },
                                         [](const MakeObj& makeObj) -> FieldAction {
                                             return MakeObj{makeObj.spec->clone()};
                                         }},
                       _data);
}
}  // namespace mongo::sbe
