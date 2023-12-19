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
    const bool isClosed = fieldsScopeIsClosed();

    if (actions.empty()) {
        actions.resize(numFieldsOfInterest);
        if (isClosed) {
            for (size_t i = 0; i < actions.size(); ++i) {
                actions[i] = Keep{};
            }
        } else {
            for (size_t i = 0; i < actions.size(); ++i) {
                actions[i] = Drop{};
            }
        }

        numFieldsOfInterest = actions.size();
        numValueArgs = 0;
        totalNumArgs = 0;
    } else {
        tassert(7103500,
                "Expected 'names' and 'fieldsInfos' to be the same size",
                names.size() == actions.size());

        for (size_t i = 0; i < actions.size(); ++i) {
            auto& action = actions[i];
            if (action.isMandatory()) {
                mandatoryFields.push_back(i);
            }
        }

        initCounters();
    }

    return StringListSet(std::move(names));
}

StringListSet MakeObjSpec::buildFieldDict(std::vector<std::string> names,
                                          const MakeObjInputPlan& inputPlan) {
    bool isClosed = fieldsScopeIsClosed();

    if (actions.empty()) {
        actions.resize(names.size());
        for (size_t i = 0; i < names.size(); ++i) {
            actions[i] = isClosed ? FieldAction{Keep{}} : FieldAction{Drop{}};
        }
    } else {
        tassert(8146600,
                "Expected 'names' and 'fieldsInfos' to be the same size",
                names.size() == actions.size());
    }

    const auto& fieldDict = inputPlan.getFieldDict();
    size_t n = fieldDict.size();
    auto newActions = std::vector<sbe::MakeObjSpec::FieldAction>(n);

    for (size_t i = 0; i < n; ++i) {
        if (!inputPlan.isFieldUsed(fieldDict[i])) {
            // For each field discarded by 'inputPlan', initialize the corresponding entry in
            // 'newActions' to "Drop".
            newActions[i] = Drop{};
        } else {
            // For each field not discardard by 'inputPlan', initialize the corresponding entry in
            // 'newActions' to "Drop" (if isClosed is true) or "Keep" (if isClosed is false).
            newActions[i] = isClosed ? FieldAction{Drop{}} : FieldAction{Keep{}};
        }
    }

    // Copy the contents of 'actions' over to 'newActions' and populate 'mandatoryFields'.
    for (size_t i = 0; i < actions.size(); ++i) {
        auto& action = actions[i];

        size_t pos = fieldDict.findPos(names[i]);
        if (pos == StringListSet::npos) {
            tassert(8146601,
                    "Expected non-dropped field from 'names' to be present in 'fieldDict'",
                    action.isDrop() && inputPlan.fieldsScopeIsClosed());
            continue;
        }

        newActions[pos] = action.clone();

        if (action.isMandatory()) {
            mandatoryFields.push_back(pos);
        }
    }

    // Update 'fieldsScope' to match 'inputPlan.getFieldsScope()'.
    fieldsScope = inputPlan.getFieldsScope();

    // Store the updated Actions vector into 'actions'.
    actions = std::move(newActions);

    // Initialize 'numInputFields'.
    numInputFields = inputPlan.numSingleFields();

    // Initialize 'displayOrder'. First we add all the original fields in their original order
    // and then we add the rest of the fields from the updated Actions vector (skipping any
    // fields with "default behavior").
    absl::flat_hash_set<size_t> displayOrderSet;

    for (size_t i = 0; i < names.size(); ++i) {
        size_t pos = fieldDict.findPos(names[i]);
        if (pos != StringListSet::npos) {
            auto& action = actions[pos];

            if (isClosed ? !action.isDrop() : !action.isKeep()) {
                displayOrderSet.emplace(pos);
                displayOrder.push_back(pos);
            }
        }
    }

    for (size_t pos = 0; pos < actions.size(); ++pos) {
        if (!displayOrderSet.count(pos)) {
            auto& action = actions[pos];

            if (isClosed ? !action.isDrop() : !action.isKeep()) {
                displayOrder.push_back(pos);
            }
        }
    }

    initCounters();

    return fieldDict;
}

void MakeObjSpec::initCounters() {
    const bool isClosed = fieldsScopeIsClosed();

    numFieldsOfInterest = 0;
    numValueArgs = 0;
    totalNumArgs = 0;

    for (size_t i = 0; i < actions.size(); ++i) {
        if (actions[i].isKeep() || actions[i].isDrop()) {
            numFieldsOfInterest += static_cast<uint8_t>(isClosed == actions[i].isKeep());
            continue;
        }

        ++numFieldsOfInterest;

        if (actions[i].isValueArg()) {
            ++numValueArgs;
            ++totalNumArgs;
        } else if (actions[i].isLambdaArg()) {
            ++totalNumArgs;
        } else if (actions[i].isMakeObj()) {
            totalNumArgs += actions[i].getMakeObjSpec()->totalNumArgs;
        }
    }
}

std::string MakeObjSpec::toString() const {
    const bool isClosed = fieldsScopeIsClosed();

    StringBuilder builder;
    builder << "[";

    bool hasDisplayOrder = !displayOrder.empty();
    size_t n = hasDisplayOrder ? displayOrder.size() : fields.size();

    bool first = true;
    for (size_t i = 0; i < n; ++i) {
        size_t pos = hasDisplayOrder ? displayOrder[i] : i;

        auto& name = fields[pos];
        auto& action = actions[pos];

        if ((action.isKeep() || action.isDrop()) && isClosed == action.isDrop()) {
            continue;
        }

        if (!first) {
            builder << ", ";
        } else {
            first = false;
        }

        builder << name;

        if (!action.isKeep() && !action.isDrop()) {
            if (action.isValueArg()) {
                builder << " = Arg(" << action.getValueArgIdx() << ")";
            } else if (action.isLambdaArg()) {
                const auto& lambdaArg = action.getLambdaArg();
                builder << " = LambdaArg(" << lambdaArg.argIdx
                        << (lambdaArg.returnsNothingOnMissingInput ? "" : ", false") << ")";
            } else if (action.isMakeObj()) {
                auto spec = action.getMakeObjSpec();
                builder << " = MakeObj(" << spec->toString() << ")";
            }
        }
    }

    builder << "], ";

    if (numInputFields) {
        size_t n = *numInputFields;

        builder << "[";

        bool first = true;
        for (size_t i = 0; i < n; ++i) {
            if (!first) {
                builder << ", ";
            } else {
                first = false;
            }

            builder << fields[i];
        }

        builder << "], ";
    }

    builder << (isClosed ? "Closed" : "Open");

    if (nonObjInputBehavior == NonObjInputBehavior::kReturnNothing) {
        builder << ", RetNothing";
    } else if (nonObjInputBehavior == NonObjInputBehavior::kReturnInput) {
        builder << ", RetInput";
    } else if (traversalDepth.has_value()) {
        builder << ", NewObj";
    }

    if (traversalDepth.has_value()) {
        builder << ", " << *traversalDepth;
    }

    return builder.str();
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
    return visit(OverloadedVisitor{[](Keep k) -> FieldAction { return k; },
                                   [](Drop d) -> FieldAction { return d; },
                                   [](ValueArg va) -> FieldAction { return va; },
                                   [](LambdaArg la) -> FieldAction { return la; },
                                   [](const MakeObj& makeObj) -> FieldAction {
                                       return MakeObj{makeObj.spec->clone()};
                                   }},
                 _data);
}
}  // namespace mongo::sbe
