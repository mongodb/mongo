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

#include "mongo/db/query/optimizer/rewrites/proj_spec_builder.h"
#include "mongo/db/query/optimizer/node.h"
#include "mongo/db/query/optimizer/syntax/expr.h"
#include "mongo/db/query/optimizer/syntax/path.h"

namespace mongo::optimizer {

using MakeObjSpec = sbe::MakeObjSpec;
using FieldAction = MakeObjSpec::FieldAction;
using Keep = MakeObjSpec::Keep;
using Drop = MakeObjSpec::Drop;
using LambdaArg = MakeObjSpec::LambdaArg;
using MakeObj = MakeObjSpec::MakeObj;
using ValueArg = MakeObjSpec::ValueArg;
using NonObjInputBehavior = MakeObjSpec::NonObjInputBehavior;

namespace {

/**
 * Helper function which determines which field behavior to select for the translation of a
 * 'PathComposeM'. The priority order (descending) is: 'kClosed', 'kOpen', boost::none.
 */
FieldListScope pickFieldListScope(FieldListScope l, FieldListScope r) {
    if (l != r) {
        // Both of these operations are a set difference on Keep/Drops; however, for other field
        // actions that are not keep/drops, we just keep anything in the closed set.
        return FieldListScope::kClosed;
    }
    return l;
}

/**
 * Helper function which determines which field behavior to select for the translation of a
 * 'PathComposeM'. The priority order (descending) is: 'kNewObj', 'kReturnNothing', 'kReturnInput',
 * boost::none.
 */
NonObjInputBehavior pickNonObjInputBehavior(NonObjInputBehavior l, NonObjInputBehavior r) {
    if (l != r) {
        if (l == NonObjInputBehavior::kNewObj || r == NonObjInputBehavior::kNewObj) {
            return NonObjInputBehavior::kNewObj;
        } else if (l == NonObjInputBehavior::kReturnNothing ||
                   r == NonObjInputBehavior::kReturnNothing) {
            return NonObjInputBehavior::kReturnNothing;
        }
        return NonObjInputBehavior::kReturnInput;
    }
    return l;
}
}  // namespace

bool FieldActionBuilder::absorb(FieldActionBuilder&& other,
                                boost::optional<FieldListScope> lFieldListScope = boost::none,
                                boost::optional<FieldListScope> rFieldListScope = boost::none) {
    auto copyOther = [&]() {
        std::swap(_path, other._path);
        std::swap(_innerBuilder, other._innerBuilder);
        std::swap(_isLambda, other._isLambda);
    };

    if (other.isValueArg()) {
        // By ConstCompose, p * Const c = c, so we should pick the right ValueArg.
        copyOther();
        return true;

    } else if (isKeepOrDrop()) {
        // 'fieldListScope' must be initialized if we're here.
        if (*lFieldListScope == FieldListScope::kClosed) {
            // Return right branch if Keep on the left.
            copyOther();

        } else if (other.isLambdaArg() || other.isMakeObj()) {
            // We want to fallback to lowering.
            return false;
        }

        // Otherwise a no-op.
        return true;

    } else if (other.isKeepOrDrop()) {
        // 'fieldListScope' must be initialized if we're here.
        if (*rFieldListScope == FieldListScope::kOpen) {
            // Return right branch if Drop on the right.
            copyOther();
        }  // Otherwise a no-op (keep left branch).
        return true;

    } else if (isMakeObj() && other.isMakeObj()) {
        // Recursive case!
        return _innerBuilder->absorb(std::move(other._innerBuilder));
    }

    // Bail out and trust to lowering.
    return false;
}

FieldAction FieldActionBuilder::build(FieldListScope fieldListScope, ABTVector& args) {
    if (!_path && !_innerBuilder) {
        return fieldListScope == FieldListScope::kOpen ? FieldAction(Drop{}) : FieldAction(Keep{});

    } else if (_path) {
        auto va = _isLambda
            ? FieldAction(LambdaArg{args.size(), false /* returnsNothingOnMissingInput */})
            : FieldAction(ValueArg{args.size()});
        args.push_back(*_path);
        return va;

    } else if (_innerBuilder) {
        return MakeObj{_innerBuilder->build(args)};
    }

    MONGO_UNREACHABLE_TASSERT(7936705);
}

std::unique_ptr<sbe::MakeObjSpec> ProjSpecBuilder::build(ABTVector& argStack) {
    auto& v = get<Valid>(_state);

    std::vector<std::string> fields;
    fields.reserve(v.namedFabs.size());
    std::vector<FieldAction> fieldActions;
    fieldActions.reserve(v.namedFabs.size());
    for (auto& [f, fab] : v.namedFabs) {
        fields.push_back(f);
        fieldActions.push_back(fab.build(v.fieldListScope, argStack));
    }

    return std::make_unique<sbe::MakeObjSpec>(v.fieldListScope,
                                              std::move(fields),
                                              std::move(fieldActions),
                                              v.nonObjInputBehavior,
                                              v.traversalDepth);
}

bool ProjSpecBuilder::absorb(std::unique_ptr<ProjSpecBuilder> other) {
    // We also can't merge builders if they aren't both in the same state of validity.
    auto valid = isValid();
    if (valid != other->isValid()) {
        return false;
    }

    // Either both builders need paths or neither does.
    if (!valid && needsPath() && other->needsPath()) {
        // Merge orphaned FieldActions via the same logic as FieldActions on duplicate paths.
        auto& left = get<NeedsPath>(_state).orphan;
        auto& right = get<NeedsPath>(other->_state).orphan;
        return left.absorb(std::move(right));

    } else if (!valid) {
        // Neither is valid, but at least one of them doesn't correspond to an orphaned fieldAction.
        // We should bail out.
        return false;

    }  // Otherwise, both are valid. We may proceed.

    auto& left = get<Valid>(_state);
    auto& right = get<Valid>(other->_state);

    // We can't merge builders if they traverse to different depths.
    if (left.traversalDepth != right.traversalDepth) {
        return false;
    }

    // We cannot merge builders if we have kNewObj on the right side but a different non-object
    // input behavior on the left. This is because this path means that left side's "object-only"
    // behaviors will only be applied if the input is an object, and then the right side will return
    // an empty new object if the input is a scalar. This is not directly expressible in
    // MakeObjSpec.
    if (left.nonObjInputBehavior != NonObjInputBehavior::kNewObj &&
        right.nonObjInputBehavior == NonObjInputBehavior::kNewObj) {
        return false;
    }

    // Determine output field and non-object behavior and merging strategy.
    auto fieldListScope = pickFieldListScope(left.fieldListScope, right.fieldListScope);
    auto nonObjInputBehavior =
        pickNonObjInputBehavior(left.nonObjInputBehavior, right.nonObjInputBehavior);

    // We can now apply the set operation selected above to the fields and fieldActionBuilders of
    // the two branches.
    NamedFieldActionBuilders namedFabs;

    // Set of indices of fields in R that we should skip.
    std::set<size_t> rightFieldsToIgnore;

    for (auto& [lField, lFab] : left.namedFabs) {
        // Check if we have a matching field on the right side.
        bool matching = false;
        for (size_t r = 0; r < right.namedFabs.size(); r++) {
            auto& [rField, rFab] = right.namedFabs[r];
            if (lField == rField) {
                matching = true;
                // We will process this field now, no need to re-process it later.
                rightFieldsToIgnore.insert(r);

                // If we have a Drop on the right side, and the left field behavior is kClosed, we
                // can just eliminate this field from the list.
                if (left.fieldListScope == FieldListScope::kClosed &&
                    right.fieldListScope == FieldListScope::kOpen && rFab.isKeepOrDrop()) {
                    break;
                }

                // Similarly, if we're dropping a field on the left, and keeping the same field on
                // the right, we should eliminate it from the list altogether, since the output
                // field behavior will be 'kClosed'.
                if (left.fieldListScope == FieldListScope::kOpen &&
                    right.fieldListScope == FieldListScope::kClosed && lFab.isKeepOrDrop() &&
                    rFab.isKeepOrDrop()) {
                    break;
                }

                // Otherwise, try to merge the builders for this field or fallback.
                if (lFab.absorb(std::move(rFab), left.fieldListScope, right.fieldListScope)) {
                    // We successfully merged the builer- add the field/builder to the
                    // output.
                    namedFabs.emplace_back(std::move(lField), std::move(lFab));
                } else {
                    // Fallback to lowering.
                    return false;
                }

                // Found a matching field.
                break;
            }
        }

        if (!matching && right.fieldListScope == FieldListScope::kOpen) {
            // This field only occurs on the left side, and the right side should preserve it.
            // We want to include it in the output spec.
            // Note: we never move twice here, because we only enter this block if we didn't match,
            // whereas the other time we std::moved, we matched.
            namedFabs.emplace_back(std::move(lField),  // NOLINT(bugprone-use-after-move)
                                   std::move(lFab));   // NOLINT(bugprone-use-after-move)
        }

        // If we didn't see this field on the right side, and the right side has 'fieldListScope'
        // kClosed, we should ignore it (drop it).
    }

    // Decide which remaining fields in R to include.
    for (size_t r = 0; r < right.namedFabs.size(); r++) {
        auto& [rField, rFab] = right.namedFabs[r];
        if (rightFieldsToIgnore.contains(r)) {
            // We have already processed/absorbed this field, or we just want to eliminate it.
            continue;
        } else if (left.fieldListScope == FieldListScope::kClosed) {
            if (rFab.isKeepOrDrop()) {
                // We didn't encounter this field among the left field actions, and the left branch
                // would have dropped it first. Ignore it on the right side.
                continue;
            } else if (!rFab.isValueArg()) {
                // We want to fallback if this field is either a LambdaArg or a MakeObj, because in
                // either case, we know the input will always be Nothing to this path. We could
                // create a new ValueArg node here where we pass Nothing as input to the path
                // represented by the FieldActionBuilder, but for now it is easier to fall back.
                return false;
            }
        }
        // Note that if the left side has FieldListScope::kClosed and right field is a ValueArg,
        // we want to set that field anyway. Proceed to add field/builder to output.
        namedFabs.emplace_back(std::move(rField), std::move(rFab));
    }

    left.namedFabs = std::move(namedFabs);
    left.fieldListScope = fieldListScope;
    left.nonObjInputBehavior = nonObjInputBehavior;

    return true;
}

}  // namespace mongo::optimizer
