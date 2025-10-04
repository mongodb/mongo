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

#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/util/field_set.h"
#include "mongo/util/string_listset.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace mongo::sbe {
struct MakeObjSpec {
    static constexpr size_t npos = StringListSet::npos;

    enum class NonObjInputBehavior { kReturnNothing, kReturnInput, kNewObj };
    enum class ActionType : uint8_t { kKeep, kDrop, kSetArg, kAddArg, kLambdaArg, kMakeObj };

    struct Keep {
        bool operator==(const Keep& other) const = default;
    };

    struct Drop {
        bool operator==(const Drop& other) const = default;
    };

    struct SetArg {
        bool operator==(const SetArg& other) const = default;

        size_t argIdx;
    };

    struct AddArg {
        bool operator==(const AddArg& other) const = default;

        size_t argIdx;
    };

    struct LambdaArg {
        bool operator==(const LambdaArg& other) const = default;

        size_t argIdx;
        bool returnsNothingOnMissingInput;
    };

    struct MakeObj {
        bool operator==(const MakeObj& other) const {
            return *spec == *other.spec;
        }

        std::unique_ptr<MakeObjSpec> spec;
    };

    /**
     * This class holds info about what action should be taken for a given field. Each FieldAction
     * can be one of the following:
     *   1) Keep:      Copy the field.
     *   2) Drop:      Ignore the field.
     *   3) SetArg:    Set field to the corresponding arg value, preserving the field's original
     *                 position if it already existed.
     *   4) AddArg:    Drop any existing field with the same name and add a new field at the end of
     *                 the object with corresponding arg value.
     *   5) LambdaArg: Pass the current value of the field (or Nothing if the field doesn't exist)
     *                 to the corresponding lambda arg, and then replace the field with the return
     *                 value of the lambda.
     *   6) MakeObj:   Recursively invoke makeBsonObj() passing in the field as the input object,
     *                 and replace the field with output produced.
     */
    class FieldAction {
    public:
        using Type = ActionType;

        using VariantType = std::variant<Keep, Drop, SetArg, AddArg, LambdaArg, MakeObj>;

        FieldAction() = default;

        FieldAction(Keep) : _data(Keep{}) {}
        FieldAction(Drop) : _data(Drop{}) {}
        FieldAction(SetArg setArg) : _data(setArg) {}
        FieldAction(AddArg addArg) : _data(addArg) {}
        FieldAction(LambdaArg lambdaArg) : _data(lambdaArg) {}
        FieldAction(MakeObj makeObj) : _data(std::move(makeObj)) {}

        FieldAction(std::unique_ptr<MakeObjSpec> spec) : _data(MakeObj{std::move(spec)}) {}

        FieldAction clone() const;

        Type type() const {
            return visit(OverloadedVisitor{[](Keep) { return Type::kKeep; },
                                           [](Drop) { return Type::kDrop; },
                                           [](SetArg) { return Type::kSetArg; },
                                           [](AddArg) { return Type::kAddArg; },
                                           [](LambdaArg) { return Type::kLambdaArg; },
                                           [](const MakeObj&) {
                                               return Type::kMakeObj;
                                           }},
                         _data);
        }

        bool isKeep() const {
            return holds_alternative<Keep>(_data);
        }
        bool isDrop() const {
            return holds_alternative<Drop>(_data);
        }
        bool isSetArg() const {
            return holds_alternative<SetArg>(_data);
        }
        bool isAddArg() const {
            return holds_alternative<AddArg>(_data);
        }
        bool isLambdaArg() const {
            return holds_alternative<LambdaArg>(_data);
        }
        bool isMakeObj() const {
            return holds_alternative<MakeObj>(_data);
        }

        size_t getSetArgIdx() const {
            return get<SetArg>(_data).argIdx;
        }
        size_t getAddArgIdx() const {
            return get<AddArg>(_data).argIdx;
        }
        const LambdaArg& getLambdaArg() const {
            return get<LambdaArg>(_data);
        }
        const MakeObjSpec* getMakeObjSpec() const {
            return get<MakeObj>(_data).spec.get();
        }

        bool isMandatory() const {
            return isSetArg() || isAddArg() ||
                (isLambdaArg() && !getLambdaArg().returnsNothingOnMissingInput) ||
                (isMakeObj() && !getMakeObjSpec()->returnsNothingOnMissingInput());
        }

        bool operator==(const FieldAction& other) const = default;

        template <typename H>
        friend H AbslHashValue(H hashState, const FieldAction& fi);

    private:
        VariantType _data;
    };

    MakeObjSpec(FieldListScope fieldsScope,
                std::vector<std::string> fields,
                std::vector<FieldAction> actions = {},
                NonObjInputBehavior nonObjInputBehavior = NonObjInputBehavior::kNewObj,
                boost::optional<int32_t> traversalDepth = boost::none)
        : fieldsScope(fieldsScope),
          nonObjInputBehavior(nonObjInputBehavior),
          traversalDepth(traversalDepth),
          actions(std::move(actions)),
          fields(buildFieldDict(std::move(fields))) {
        init();
    }

    MakeObjSpec(const MakeObjSpec& other)
        : fieldsScope(other.fieldsScope),
          nonObjInputBehavior(other.nonObjInputBehavior),
          traversalDepth(other.traversalDepth),
          actions(other.cloneActions()),
          numFieldsToSearchFor(other.numFieldsToSearchFor),
          totalNumArgs(other.totalNumArgs),
          mandatoryFields(std::move(other.mandatoryFields)),
          displayOrder(other.displayOrder),
          fields(other.fields) {}

    MakeObjSpec(MakeObjSpec&& other)
        : fieldsScope(other.fieldsScope),
          nonObjInputBehavior(other.nonObjInputBehavior),
          traversalDepth(other.traversalDepth),
          actions(std::move(other.actions)),
          numFieldsToSearchFor(other.numFieldsToSearchFor),
          totalNumArgs(other.totalNumArgs),
          mandatoryFields(std::move(other.mandatoryFields)),
          displayOrder(std::move(other.displayOrder)),
          fields(std::move(other.fields)) {}

    MakeObjSpec& operator=(const MakeObjSpec& other) = delete;

    MakeObjSpec& operator=(MakeObjSpec&&) = delete;

    std::unique_ptr<MakeObjSpec> clone() const {
        return std::make_unique<MakeObjSpec>(*this);
    }

    bool fieldsScopeIsClosed() const {
        return fieldsScope == FieldListScope::kClosed;
    }

    bool fieldsScopeIsOpen() const {
        return fieldsScope == FieldListScope::kOpen;
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

    std::string toString() const;

    bool operator==(const MakeObjSpec& other) const = default;

    size_t getApproximateSize() const;

    const FieldAction* getActionsData() const {
        return !actions.empty() ? actions.data() : &_singleKeepAction[0];
    }

private:
    StringListSet buildFieldDict(std::vector<std::string> names);

    void init();

    static FieldAction _singleKeepAction[1];

public:
    // 'fieldsScope' indicates how other fields not present in 'fields' should be handled.
    // If 'fieldsScope == kOpen', then other fields not present in 'fields' should be copied
    // to the output object. If 'fieldsScope == kClosed', then other fields not present in
    // 'fields' should be ignored/dropped.
    FieldListScope fieldsScope = FieldListScope::kOpen;

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
    // (i.e. the info corresponding to field[i] is stored in actions[i]). 'actions' is initially
    // set to the std::vector<FieldAction> passed into the constructor and then buildFieldDict()
    // updates the contents of 'actions' as appropriate.
    std::vector<FieldAction> actions;

    /**
     * The fields below are all initialized by buildFieldDict().
     */

    // Number of distinct field names that are being "searched for" during the scan phase.
    size_t numFieldsToSearchFor = 0;

    // The total number of SetArgs, AddArgs, and LamdbaArgs in this MakeObjSpec and all of its
    // descendents.
    size_t totalNumArgs = 0;

    // List of idxs of the fields that have mandatory actions. When makeBsonObj() reaches the end
    // of the input object, it will perform the actions in this list in order they appear for each
    // field that wasn't present in the input object.
    std::vector<size_t> mandatoryFields;

    std::vector<size_t> displayOrder;

    // This StringListSet provides the field name->idx mapping that is used throughout this
    // data structure. For a given field F that is present in 'fields', the idx of the field
    // can be computed by doing 'idx = fields.findPos(F)', and the corresponding action for
    // field F can be retrieved by using 'idx' to index into the 'actions' vector.
    StringListSet fields;

    template <typename H>
    friend H AbslHashValue(H hashState, const MakeObjSpec& mos);
};

template <typename H>
H AbslHashValue(H hashState, const MakeObjSpec& mos) {
    hashState = H::combine(std::move(hashState), mos.fieldsScope, mos.nonObjInputBehavior);
    if (mos.traversalDepth) {
        hashState = H::combine(std::move(hashState), *mos.traversalDepth);
    }
    for (size_t i = 0; i < mos.fields.size(); i++) {
        hashState = H::combine(std::move(hashState), mos.fields[i], mos.actions[i]);
    }
    return hashState;
}

template <typename H>
H AbslHashValue(H hashState, const MakeObjSpec::FieldAction& fi) {
    // Hash the index of the variant alternative to differentiate the different types in the hash.
    hashState = H::combine(std::move(hashState), fi._data.index());

    if (fi.isSetArg()) {
        hashState = H::combine(std::move(hashState), fi.getSetArgIdx());
    } else if (fi.isAddArg()) {
        hashState = H::combine(std::move(hashState), fi.getAddArgIdx());
    } else if (fi.isLambdaArg()) {
        const auto& lambdaArg = fi.getLambdaArg();
        hashState = H::combine(
            std::move(hashState), lambdaArg.argIdx, lambdaArg.returnsNothingOnMissingInput);
    } else if (fi.isMakeObj()) {
        hashState = H::combine(std::move(hashState), *(fi.getMakeObjSpec()));
    }

    // If Keep{} or Drop{}, don't update hash state, as the variant index() will differentiate them.
    return hashState;
}

}  // namespace mongo::sbe
