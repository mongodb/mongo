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

#pragma once

#include "mongo/db/exec/sbe/makeobj_spec.h"
#include "mongo/db/query/optimizer/syntax/syntax.h"
#include <boost/optional/optional.hpp>
#include <utility>

namespace mongo::optimizer {

class ProjSpecBuilder;

/**
 * Builder to assist in the construction of FieldActions.
 *
 * Just like a FieldAction, it represents a function from value (or Nothing) to value (or Nothing).
 * Also like FieldAction it does not distinguish between Keep and Drop, so you also need to know
 * a FieldListScope to disambiguate.
 *
 * However, in the case of LambdaArg and ValueArg, it does not track the index of the argument it
 * points to, but rather it may keep a pointer to a node in the ABT tree used to generate it. This
 * allows us to avoid materializing and lowering the arguments we will need for the final "makeObj"
 * primitive until we have completely constructed the MakeObjSpec. This is useful if, for example,
 * we can eliminate some ValueArgs on different sides of a composition. It also allows us to defer
 * lowering any part of the tree until we know exactly which subtrees need to be lowered (if any).
 */
class FieldActionBuilder {
public:
    // MakeObj constructor.
    FieldActionBuilder(std::unique_ptr<ProjSpecBuilder> builder)
        : _innerBuilder(std::move(builder)) {}

    // ValueArg/LambdaArg constructor.
    FieldActionBuilder(const ABT* path, bool isLambda) : _isLambda(isLambda), _path(path) {}

    // KeepDrop constructor.
    FieldActionBuilder() {}

    FieldActionBuilder(FieldActionBuilder&& other)
        : _isLambda(other._isLambda),
          _path(other._path),
          _innerBuilder(other._innerBuilder.release()) {}

    // No copies.
    FieldActionBuilder(const FieldActionBuilder& other) = delete;

    // Helper used for combining two builders during path composition translation. This is called
    // when we have a PathComposeM of two paths. It applies path composition rules
    // to try to combine the behavior encoded by the left (this) side/FieldActionBuilder followed by
    // the right
    // ('other') side/FieldActionBuilder into a single FieldActionBuilder. If it is successful, it
    // returns true and modifies 'this' into the equivalent behavior. Otherwise, it returns false/
    // nothing is modified.
    bool absorb(FieldActionBuilder&& other,
                boost::optional<FieldListScope> lFieldListScope,
                boost::optional<FieldListScope> rFieldListScope);

    bool isKeepOrDrop() const {
        return !_path && !_innerBuilder;
    }

    bool isValueArg() const {
        return _path && !_innerBuilder && !_isLambda;
    }

    bool isLambdaArg() const {
        return _path && !_innerBuilder && _isLambda;
    }

    bool isMakeObj() const {
        return !_path && _innerBuilder;
    }

    // Builder method that generates the final FieldAction and populates 'args' with its '_path' if
    // necessary. Note that we also need to know the scope in order to generate Keep{} or Drop{} as
    // appropriate.
    sbe::MakeObjSpec::FieldAction build(FieldListScope fieldListScope, ABTVector& args);

private:
    bool _isLambda = false;
    const ABT* _path = nullptr;
    std::unique_ptr<ProjSpecBuilder> _innerBuilder = nullptr;
};

/**
 * Builder to assist in the construction of a MakeObjSpec.
 */
using NamedFieldActionBuilders = std::vector<std::pair<std::string, FieldActionBuilder>>;
class ProjSpecBuilder {
    struct NeedsPath {
        // Orphaned FieldAction builder which needs to be assigned a path in order to become a valid
        // builder.
        FieldActionBuilder orphan;
    };

    struct Valid {
        Valid(sbe::MakeObjSpec::NonObjInputBehavior nonObjInputBehavior,
              FieldListScope fieldListScope)
            : nonObjInputBehavior(nonObjInputBehavior), fieldListScope(fieldListScope) {}

        // Enum describing what to do when the input type is not an object.
        sbe::MakeObjSpec::NonObjInputBehavior nonObjInputBehavior;

        // Enum describing what to do upon encountering fields not listed in '_namedFields'.
        FieldListScope fieldListScope;

        // Vector describing what to do with each field.
        NamedFieldActionBuilders namedFabs;

        // Defaults to 0 unless we see a traverse; only applies for 'MakeObj' FieldActions.
        // This corresponds to the absence of a 'Traverse' on the current path.
        boost::optional<int32_t> traversalDepth = 0;

        bool isTrivial() const {
            return namedFabs.size() == 0 && fieldListScope == FieldListScope::kOpen &&
                nonObjInputBehavior == sbe::MakeObjSpec::NonObjInputBehavior::kReturnInput;
        }
    };

public:
    // Creates a blank builder with only behaviors set. Note that this yields a valid MakeObjSpec.
    ProjSpecBuilder(sbe::MakeObjSpec::NonObjInputBehavior nonObjInputBehavior,
                    FieldListScope fieldListScope)
        : _state(Valid(nonObjInputBehavior, fieldListScope)) {}

    // Creates a FieldAction with no field/behaviors specified yet that needs a path.
    ProjSpecBuilder(FieldActionBuilder fab) : _state(NeedsPath{std::move(fab)}) {}

    // KeepDrop constructor: used to construct simple inclusion/exclusion projections.
    ProjSpecBuilder(FieldListScope fieldListScope, std::vector<std::string> fields)
        : _state(Valid(sbe::MakeObjSpec::NonObjInputBehavior::kReturnInput, fieldListScope)) {
        auto& valid = get<Valid>(_state);
        valid.namedFabs.reserve(fields.size());
        for (const auto& field : fields) {
            valid.namedFabs.emplace_back(field, FieldActionBuilder());
        }
    }

    // No copies.
    ProjSpecBuilder(const ProjSpecBuilder& other) = delete;

    void setCurrentPath(std::string currentPath, sbe::MakeObjSpec::NonObjInputBehavior behavior) {
        tassert(7936700, "Cannot override current path", needsPath());
        auto& np = get<NeedsPath>(_state);
        Valid v(behavior, FieldListScope::kOpen);
        v.namedFabs.emplace_back(currentPath, std::move(np.orphan));
        _state = std::move(v);
    }

    void setTraversalDepth(boost::optional<int32_t> depth) {
        get<Valid>(_state).traversalDepth = std::move(depth);
    }

    // Helper to combine this builder with another when translating a PathComposeM. It encodes the
    // semantics of first applying the left branch of the composition (represented as 'this'
    // builder) to the input, followed by the right branch (represented as the 'other' builder).
    // This follows the path composition laws, returning false if we cannot encode this composition
    // directly in MakeObjSpec. If successful, it returns true and modifies the 'this' builder to
    // encode the MakeObjSpec equivalent of the entire PathComposeM.
    bool absorb(std::unique_ptr<ProjSpecBuilder> other);

    bool isValid() const {
        return holds_alternative<Valid>(_state);
    }

    // Used to determine if we should avoid placing a make obj primitive because lowering and
    // const-folding would simplify this expression.
    bool isTrivial() const {
        return isValid() && get<Valid>(_state).isTrivial();
    }

    bool needsPath() const {
        return holds_alternative<NeedsPath>(_state);
    }

    auto traversalDepth() const {
        return get<Valid>(_state).traversalDepth;
    }

    sbe::MakeObjSpec::NonObjInputBehavior nonObjInputBehavior() {
        return get<Valid>(_state).nonObjInputBehavior;
    }

    // Builder method generating final MakeObjSpec. May only be called if isValid() evaluates to
    // true. May recursively call build() on nested builders, and will populate 'argStack' with
    // arguments that _fieldActionBuilders reference.
    std::unique_ptr<sbe::MakeObjSpec> build(ABTVector& argStack);

private:
    std::variant<NeedsPath, Valid> _state;
};
}  // namespace mongo::optimizer
