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

#include "mongo/db/exec/sbe/makeobj_spec.h"

#include "mongo/db/exec/sbe/size_estimator.h"

#include <algorithm>
#include <iterator>

namespace mongo::sbe {
MakeObjSpec::FieldAction MakeObjSpec::_singleKeepAction[1] = {
    MakeObjSpec::FieldAction{MakeObjSpec::Keep{}}};

StringListSet MakeObjSpec::buildFieldDict(std::vector<std::string> names) {
    const bool isClosed = fieldsScopeIsClosed();

    if (actions.empty()) {
        actions.resize(names.size());

        if (isClosed) {
            for (size_t i = 0; i < actions.size(); ++i) {
                actions[i] = Keep{};
            }
        } else {
            for (size_t i = 0; i < actions.size(); ++i) {
                actions[i] = Drop{};
            }
        }
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
    }

    return StringListSet(std::move(names));
}

void MakeObjSpec::init() {
    // Initialize 'numFieldsToSearchFor' and 'totalNumArgs'.
    const bool isClosed = fieldsScopeIsClosed();

    totalNumArgs = 0;
    numFieldsToSearchFor = 0;

    for (const auto& action : actions) {
        // Increment 'totalNumArgs'.
        if (action.isSetArg() || action.isAddArg() || action.isLambdaArg()) {
            ++totalNumArgs;
        } else if (action.isMakeObj()) {
            totalNumArgs += action.getMakeObjSpec()->totalNumArgs;
        }

        // Increment 'numFieldsToSearchFor'.
        if (action.isKeep()) {
            numFieldsToSearchFor += static_cast<uint8_t>(isClosed);
        } else if (action.isDrop() || action.isAddArg()) {
            numFieldsToSearchFor += static_cast<uint8_t>(!isClosed);
        } else {
            ++numFieldsToSearchFor;
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
            builder << " = ";

            if (action.isSetArg()) {
                builder << "Set(" << action.getSetArgIdx() << ")";
            } else if (action.isAddArg()) {
                builder << "Add(" << action.getAddArgIdx() << ")";
            } else if (action.isLambdaArg()) {
                const auto& lambdaArg = action.getLambdaArg();
                builder << "Lambda(" << lambdaArg.argIdx
                        << (lambdaArg.returnsNothingOnMissingInput ? "" : ", false") << ")";
            } else if (action.isMakeObj()) {
                auto spec = action.getMakeObjSpec();
                builder << "MakeObj(" << spec->toString() << ")";
            }
        }
    }

    builder << "], ";

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

    for (const auto& action : actions) {
        if (action.isMakeObj()) {
            size += action.getMakeObjSpec()->getApproximateSize();
        }
    }

    return size;
}

MakeObjSpec::FieldAction MakeObjSpec::FieldAction::clone() const {
    return visit(OverloadedVisitor{[](Keep k) -> FieldAction { return k; },
                                   [](Drop d) -> FieldAction { return d; },
                                   [](SetArg sa) -> FieldAction { return sa; },
                                   [](AddArg aa) -> FieldAction { return aa; },
                                   [](LambdaArg la) -> FieldAction { return la; },
                                   [](const MakeObj& makeObj) -> FieldAction {
                                       return MakeObj{makeObj.spec->clone()};
                                   }},
                 _data);
}
}  // namespace mongo::sbe
