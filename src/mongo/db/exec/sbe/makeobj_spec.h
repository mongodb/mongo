/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/util/string_listset.h"

namespace mongo::sbe {
enum class FieldListScope { kClosed, kOpen };

/**
 * MakeObjSpec is a wrapper around a FieldBehavior value and a list of field names / project names.
 */
struct MakeObjSpec {
    using FieldBehavior = FieldListScope;
    enum class NonObjInputBehavior { kReturnNothing, kReturnInput, kNewObj };

    struct KeepOrDrop {};
    struct ValueArg {
        size_t argIdx;
    };
    struct LambdaArg {
        size_t argIdx;
        bool returnsNothingOnMissingInput;
    };
    struct MakeObj {
        std::unique_ptr<MakeObjSpec> spec;
    };

    /**
     * This class holds info about what action should be taken for a given field. Each FieldAction
     * can be one of the following:
     *   1) KeepOrDrop: If 'fieldBehavior == kClosed' is true then copy the field, otherwise ignore
     *                  the field.
     *   2) ValueArg:   If the field exists then overwrite it with the corresponding argument,
     *                  otherwise add a new field set to the corresponding argument.
     *   3) LambdaArg:  Pass the current value of the field (or Nothing if the field doesn't exist)
     *                  to the corresponding lambda arg, and then replace the field with the return
     *                  value of the lambda.
     *   4) MakeObj:    Recursively invoke makeBsonObj() passing in the field as the input object,
     *                  and replace the field with output produced.
     */
    class FieldAction {
    public:
        using VariantType = stdx::variant<KeepOrDrop, ValueArg, LambdaArg, MakeObj>;

        FieldAction() = default;
        FieldAction(size_t valueArgIdx) : _data(ValueArg{valueArgIdx}) {}
        FieldAction(std::unique_ptr<MakeObjSpec> spec) : _data(MakeObj{std::move(spec)}) {}

        FieldAction(KeepOrDrop) : _data(KeepOrDrop{}) {}
        FieldAction(ValueArg valueArg) : _data(valueArg) {}
        FieldAction(LambdaArg lambdaArg) : _data(lambdaArg) {}
        FieldAction(MakeObj makeObj) : _data(std::move(makeObj)) {}

        FieldAction clone() const;

        bool isKeepOrDrop() const {
            return stdx::holds_alternative<KeepOrDrop>(_data);
        }
        bool isValueArg() const {
            return stdx::holds_alternative<ValueArg>(_data);
        }
        bool isLambdaArg() const {
            return stdx::holds_alternative<LambdaArg>(_data);
        }
        bool isMakeObj() const {
            return stdx::holds_alternative<MakeObj>(_data);
        }

        size_t getValueArgIdx() const {
            return stdx::get<ValueArg>(_data).argIdx;
        }
        const LambdaArg& getLambdaArg() const {
            return stdx::get<LambdaArg>(_data);
        }
        MakeObjSpec* getMakeObjSpec() const {
            return stdx::get<MakeObj>(_data).spec.get();
        }

    private:
        VariantType _data;
    };

    MakeObjSpec(FieldBehavior fieldBehavior,
                std::vector<std::string> fields,
                std::vector<FieldAction> actions = {},
                NonObjInputBehavior nonObjInputBehavior = NonObjInputBehavior::kNewObj,
                boost::optional<int32_t> traversalDepth = boost::none)
        : fieldBehavior(fieldBehavior),
          nonObjInputBehavior(nonObjInputBehavior),
          traversalDepth(traversalDepth),
          actions(std::move(actions)),
          fields(buildFieldDict(std::move(fields))) {}

    MakeObjSpec(const MakeObjSpec& other)
        : numKeepOrDrops(other.numKeepOrDrops),
          numValueArgs(other.numValueArgs),
          numMandatoryMakeObjs(other.numMandatoryMakeObjs),
          numMandatoryLambdas(other.numMandatoryLambdas),
          totalNumArgs(other.totalNumArgs),
          fieldBehavior(other.fieldBehavior),
          nonObjInputBehavior(other.nonObjInputBehavior),
          traversalDepth(other.traversalDepth),
          actions(other.cloneActions()),
          fields(other.fields) {}

    MakeObjSpec(MakeObjSpec&& other)
        : numKeepOrDrops(other.numKeepOrDrops),
          numValueArgs(other.numValueArgs),
          numMandatoryMakeObjs(other.numMandatoryMakeObjs),
          numMandatoryLambdas(other.numMandatoryLambdas),
          totalNumArgs(other.totalNumArgs),
          fieldBehavior(other.fieldBehavior),
          nonObjInputBehavior(other.nonObjInputBehavior),
          traversalDepth(other.traversalDepth),
          actions(std::move(other.actions)),
          fields(std::move(other.fields)) {}

    MakeObjSpec& operator=(const MakeObjSpec& other) = delete;

    MakeObjSpec& operator=(MakeObjSpec&&) = delete;

    std::unique_ptr<MakeObjSpec> clone() const {
        return std::make_unique<MakeObjSpec>(*this);
    }

    bool returnsNothingOnMissingInput() const {
        return nonObjInputBehavior != NonObjInputBehavior::kNewObj;
    }

    std::vector<FieldAction> cloneActions() const {
        std::vector<FieldAction> actionsCopy;
        for (auto&& info : actions) {
            actionsCopy.emplace_back(info.clone());
        }

        return actionsCopy;
    }

    std::string toString() const {
        const bool isClosed = fieldBehavior == FieldBehavior::kClosed;

        StringBuilder builder;
        builder << "[";

        for (size_t i = 0; i < fields.size(); ++i) {
            auto& info = actions[i];

            if (i != 0) {
                builder << ", ";
            }

            builder << "\"" << fields[i] << "\"";

            if (info.isKeepOrDrop()) {
                continue;
            } else {
                builder << " = ";
            }

            if (info.isValueArg()) {
                builder << "Arg(" << info.getValueArgIdx() << ")";
            } else if (info.isLambdaArg()) {
                const auto& lambdaArg = info.getLambdaArg();
                builder << "LambdaArg(" << lambdaArg.argIdx
                        << (lambdaArg.returnsNothingOnMissingInput ? "" : ", false") << ")";
            } else if (info.isMakeObj()) {
                auto spec = info.getMakeObjSpec();
                builder << "MakeObj(" << spec->toString() << ")";
            }
        }

        builder << "], " << (isClosed ? "Closed" : "Open");

        if (nonObjInputBehavior == NonObjInputBehavior::kReturnNothing) {
            builder << ", ReturnNothing";
        } else if (nonObjInputBehavior == NonObjInputBehavior::kReturnInput) {
            builder << ", ReturnInput";
        } else if (traversalDepth.has_value()) {
            builder << ", NewObj";
        }

        if (traversalDepth.has_value()) {
            builder << ", " << *traversalDepth;
        }

        return builder.str();
    }

    StringListSet buildFieldDict(std::vector<std::string> names);

    size_t getApproximateSize() const;

    // Number of 'fields' that are simple "keeps" or "drops".
    size_t numKeepOrDrops = 0;

    // Number of 'fields' that are ValueArgs.
    size_t numValueArgs = 0;

    // Number of 'fields' that are MakeObjs where 'returnsNothingOnMissingInput()' is false.
    size_t numMandatoryMakeObjs = 0;

    // Number of 'fields' that are LambdaArgs where 'returnsNothingOnMissingInput' is false.
    size_t numMandatoryLambdas = 0;

    // The total number of ValueArgs and LamdbaArgs in this MakeObjSpec and all of its descendents.
    size_t totalNumArgs = 0;

    // 'fieldBehavior' indicates how other fields not present in 'fields' should be handled.
    // If 'fieldBehavior == kOpen', then other fields not present in 'fields' should be copied
    // to the output object. If 'fieldBehavior == kClosed', then other fields not present in
    // 'fields' should be ignored/dropped.
    FieldBehavior fieldBehavior = FieldBehavior::kOpen;

    // 'nonObjInputBehavior' indicates how makeBsonObj() should behave if the input value is not an
    // object. If 'nonObjInputBehavior == kNewObj', then makeBsonObj() should replace the input
    // with an empty object and proceed as usual. If 'nonObjInputBehavior == kReturnNothing', then
    // makeBsonObj() should return Nothing. If 'nonObjInputBehavior == kReturnInput', makeBsonObj()
    // should return the input value.
    NonObjInputBehavior nonObjInputBehavior = NonObjInputBehavior::kNewObj;

    // If this MakeObjSpec is part of a "MakeObj" FieldAction, 'traversalDepth' indicates what
    // the array traversal depth limit should be. By default 'traversalDepth' is 'boost::none'
    // (i.e. by default there is no depth limit).
    boost::optional<int32_t> traversalDepth;

    // Contains info about each field of interest. 'fields' and 'actions' are parallel vectors
    // (i.e. the info corresponding to field[i] is stored in actions[i]).
    std::vector<FieldAction> actions;

    // Searchable vector of fields of interest.
    StringListSet fields;
};
}  // namespace mongo::sbe
