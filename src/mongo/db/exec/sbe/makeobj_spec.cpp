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
    if (fieldInfos.empty()) {
        numKeepOrDrops = names.size();
        numValueArgs = 0;
        numMandatoryLambdas = 0;
        numMandatoryMakeObjs = 0;
        totalNumArgs = 0;

        fieldInfos = std::vector<FieldInfo>{};
        fieldInfos.resize(numKeepOrDrops);

        return StringListSet(std::move(names));
    }

    tassert(7103500,
            "Expected 'names' and 'fieldsInfos' to be the same size",
            names.size() == fieldInfos.size());

    std::vector<std::string> keepOrDrops;

    numKeepOrDrops = 0;
    numValueArgs = 0;
    numMandatoryLambdas = 0;
    numMandatoryMakeObjs = 0;
    totalNumArgs = 0;

    size_t endPos = 0;

    for (size_t i = 0; i < names.size(); ++i) {
        if (fieldInfos[i].isKeepOrDrop()) {
            ++numKeepOrDrops;
            keepOrDrops.emplace_back(std::move(names[i]));
        } else {
            if (fieldInfos[i].isValueArg()) {
                ++numValueArgs;
            } else if (fieldInfos[i].isLambdaArg() &&
                       !fieldInfos[i].getLambdaArg().returnsNothingOnMissingInput) {
                ++numMandatoryLambdas;
            } else if (fieldInfos[i].isMakeObj() &&
                       !fieldInfos[i].getMakeObjSpec()->returnsNothingOnMissingInput()) {
                ++numMandatoryMakeObjs;
            }

            if (fieldInfos[i].isValueArg() || fieldInfos[i].isLambdaArg()) {
                ++totalNumArgs;
            } else if (fieldInfos[i].isMakeObj()) {
                totalNumArgs += fieldInfos[i].getMakeObjSpec()->totalNumArgs;
            }

            if (i != endPos) {
                names[endPos] = std::move(names[i]);
                fieldInfos[endPos] = std::move(fieldInfos[i]);
            }
            ++endPos;
        }
    }

    if (endPos != names.size()) {
        names.erase(names.begin() + endPos, names.end());
        fieldInfos.erase(fieldInfos.begin() + endPos, fieldInfos.end());
    }

    std::vector<FieldInfo> newFieldInfos;
    newFieldInfos.resize(numKeepOrDrops);

    std::move(fieldInfos.begin(), fieldInfos.end(), std::back_inserter(newFieldInfos));
    fieldInfos = std::move(newFieldInfos);

    std::vector<std::string> newNames = std::move(keepOrDrops);
    std::move(names.begin(), names.end(), std::back_inserter(newNames));

    return StringListSet(std::move(newNames));
}

size_t MakeObjSpec::getApproximateSize() const {
    auto size = sizeof(MakeObjSpec);

    size += size_estimator::estimate(fields);

    size += size_estimator::estimateContainerOnly(fieldInfos);

    for (size_t i = 0; i < fieldInfos.size(); ++i) {
        if (fieldInfos[i].isMakeObj()) {
            size += fieldInfos[i].getMakeObjSpec()->getApproximateSize();
        }
    }

    return size;
}

MakeObjSpec::FieldInfo MakeObjSpec::FieldInfo::clone() const {
    return stdx::visit(OverloadedVisitor{[](KeepOrDrop kd) -> FieldInfo { return kd; },
                                         [](ValueArg va) -> FieldInfo { return va; },
                                         [](LambdaArg la) -> FieldInfo { return la; },
                                         [](const MakeObj& makeObj) -> FieldInfo {
                                             return MakeObj{makeObj.spec->clone()};
                                         }},
                       _data);
}
}  // namespace mongo::sbe
