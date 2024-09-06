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

#include "mongo/db/query/optimizer/rewrites/proj_spec_lower.h"

#include "mongo/db/exec/sbe/makeobj_spec.h"
#include "mongo/db/query/optimizer/algebra/operator.h"
#include "mongo/db/query/optimizer/algebra/polyvalue.h"
#include "mongo/db/query/optimizer/node.h"
#include "mongo/db/query/optimizer/rewrites/proj_spec_builder.h"
#include "mongo/db/query/optimizer/syntax/expr.h"
#include "mongo/db/query/optimizer/syntax/path.h"
#include "mongo/db/query/optimizer/syntax/syntax.h"

namespace mongo::optimizer {

using MakeObjSpec = sbe::MakeObjSpec;
using NonObjInputBehavior = MakeObjSpec::NonObjInputBehavior;

namespace {
/**
 * Utility function to generate an appropriate projection spec builder for a PathKeep or PathDrop
 * node.
 */
std::unique_ptr<ProjSpecBuilder> makeKeepOrDropBuilder(FieldListScope behavior,
                                                       const FieldNameOrderedSet& names) {
    std::vector<std::string> fields;
    fields.reserve(names.size());
    for (const auto& name : names) {
        fields.push_back(name.value().toString());
    }

    return std::make_unique<ProjSpecBuilder>(behavior, std::move(fields));
}

/**
 * Utility function to generate an appropriate projection spec builder for a Path component that can
 * only be represented via a PathLambda.
 */
std::unique_ptr<ProjSpecBuilder> makeLambdaBuilder(const ABT& n) {
    return std::make_unique<ProjSpecBuilder>(FieldActionBuilder(&n, true /* isLambda */));
}

/**
 * Utility function to generate an appropriate projection spec builder for a PathConstant node.
 */
std::unique_ptr<ProjSpecBuilder> makeValueBuilder(const ABT& n) {
    return std::make_unique<ProjSpecBuilder>(FieldActionBuilder(&n, false /* isLambda */));
}

/**
 * Transport that visits a path bottom-up and tries to construct an appropriate MakeObjSpec builder
 */
class ProjSpecBuilderTransport {
public:
    template <typename T, typename... Ts>
    std::unique_ptr<ProjSpecBuilder> transport(const ABT&, const T&, Ts&&...) {
        static_assert(!std::is_base_of_v<PathSyntaxSort, T>,
                      "Path elements must define their transport");
        return nullptr;
    }
    std::unique_ptr<ProjSpecBuilder> transport(const ABT&,
                                               const PathCompare&,
                                               std::unique_ptr<ProjSpecBuilder>) {
        tasserted(7936703, "PathCompare not allowed under EvalPath");
        return nullptr;
    }

    std::unique_ptr<ProjSpecBuilder> transport(const ABT&,
                                               const PathComposeA&,
                                               std::unique_ptr<ProjSpecBuilder>,
                                               std::unique_ptr<ProjSpecBuilder>) {
        tasserted(7936704, "PathComposeA not allowed under EvalPath");
        return nullptr;
    }

    std::unique_ptr<ProjSpecBuilder> transport(const ABT& n, const PathArr&) {
        return makeLambdaBuilder(n);
    }

    std::unique_ptr<ProjSpecBuilder> transport(const ABT& n,
                                               const PathGet&,
                                               std::unique_ptr<ProjSpecBuilder>) {
        return makeLambdaBuilder(n);
    }

    std::unique_ptr<ProjSpecBuilder> transport(const ABT& n,
                                               const PathDefault&,
                                               std::unique_ptr<ProjSpecBuilder>) {
        return makeLambdaBuilder(n);
    }

    std::unique_ptr<ProjSpecBuilder> transport(const ABT&, const PathIdentity&) {
        // Returns the input if it is not an object; preserves the input (and its fields!)
        // otherwise.
        return std::make_unique<ProjSpecBuilder>(NonObjInputBehavior::kReturnInput,
                                                 FieldListScope::kOpen);
    }

    std::unique_ptr<ProjSpecBuilder> transport(const ABT&, const PathObj&) {
        // Returns 'Nothing' if input is not an object; preserves the input (and its fields!)
        // otherwise.
        return std::make_unique<ProjSpecBuilder>(NonObjInputBehavior::kReturnNothing,
                                                 FieldListScope::kOpen);
    }

    std::unique_ptr<ProjSpecBuilder> transport(const ABT&, const PathDrop& drop) {
        // We should copy fields not present in 'drop.getNames()'.
        return makeKeepOrDropBuilder(FieldListScope::kOpen, drop.getNames());
    }

    std::unique_ptr<ProjSpecBuilder> transport(const ABT&, const PathKeep& keep) {
        // We should eliminate fields not present in 'keep.getNames()'.
        return makeKeepOrDropBuilder(FieldListScope::kClosed, keep.getNames());
    }

    std::unique_ptr<ProjSpecBuilder> transport(const ABT&,
                                               const PathConstant& c,
                                               std::unique_ptr<ProjSpecBuilder>) {
        // We don't want to generate a lambda here; instead, we just want the constant expression.
        return makeValueBuilder(c.getConstant());
    }

    std::unique_ptr<ProjSpecBuilder> transport(const ABT& n,
                                               const PathLambda&,
                                               std::unique_ptr<ProjSpecBuilder>) {
        return makeLambdaBuilder(n);
    }

    std::unique_ptr<ProjSpecBuilder> transport(const ABT& n,
                                               const PathTraverse& traverse,
                                               std::unique_ptr<ProjSpecBuilder> inner) {
        if (inner && inner->isValid()) {
            boost::optional<int32_t> maxDepth;
            if (traverse.getMaxDepth() != PathTraverse::kUnlimited && inner->traversalDepth()) {
                // Infinite depth traversal is represented as boost::none in MakeObjSpec, but other
                // depths are equivalent. We add consecutive traverse depths together.
                maxDepth = traverse.getMaxDepth() + *(inner->traversalDepth());
            }
            // Note that if we have consecutive traverses and either of them is infinite, we want to
            // set the traversal depth to be unlimited.
            inner->setTraversalDepth(maxDepth);

        } else if (inner) {
            // This case could happen if we have something like Field "a" Traverse [n] Const c; the
            // inner path would result in an orphaned SetArg, but MakeObjSpec does not do traversals
            // on anything other than MakeObj, so we should fallback to lowering here.
            return makeLambdaBuilder(n);
        }

        return inner;
    }

    std::unique_ptr<ProjSpecBuilder> transport(const ABT&,
                                               const PathField& field,
                                               std::unique_ptr<ProjSpecBuilder> inner) {
        if (!inner) {
            return inner;
        }

        std::unique_ptr<ProjSpecBuilder> out;
        const auto& fieldName = field.name().value().toString();

        // PathField semantics say:
        //  1. If input is an object, set the specified field to the result of the inner path
        //  evaluated on its input, and return the object. This means we want to preserve
        //  existing fields ('kOpen').
        //  2. Otherwise, PathField either creates an object, sets the path applied to the input
        //  in that object, and returns the object, or returns nothing if the inner path yields
        //  nothing. We should determine the new path behavior on the child's, unless it is not set,
        //  in which case we should set it to kNewObj by default.
        auto behavior = NonObjInputBehavior::kNewObj;

        if (inner->needsPath()) {
            // This means the child of this path generated an orphaned 'FieldAction',
            // and did not know what its path should be. Assign it to the current field, and set the
            // non-object input behavior to 'kNewObj'.
            out = std::move(inner);

        } else if (inner->isValid()) {
            // The child generated a completed builder for MakeObjSpec. The path should be set to a
            // sub-object specified by the child.

            // If the inner path has one of the 'Nothing'-preserving behaviors 'kReturnInput' or
            // 'kReturnNothing', we should set the parent behavior to 'kReturnInput'. Otherwise, we
            // want to propagate 'kNewObj' up to the root spec.
            if (inner->nonObjInputBehavior() != NonObjInputBehavior::kNewObj) {
                behavior = NonObjInputBehavior::kReturnInput;
            }

            out = std::make_unique<ProjSpecBuilder>(FieldActionBuilder(std::move(inner)));
        }

        if (out) {
            // Note that this sets the 'FieldListScope' to kOpen.
            out->setCurrentPath(fieldName, behavior);
        }

        return out;
    }

    std::unique_ptr<ProjSpecBuilder> transport(const ABT& n,
                                               const PathComposeM& compose,
                                               std::unique_ptr<ProjSpecBuilder> p1,
                                               std::unique_ptr<ProjSpecBuilder> p2) {
        // First, check for the case where we have the toObj path Obj * Default {}.
        // We can convert this into a blank ProjSpecBuilder with the appropriate behaviors set, i.e.
        // kNewObj (return the input if it is an object, else create a new object).
        if (compose.getPath1().is<PathObj>()) {
            auto def = compose.getPath2().cast<PathDefault>();
            if (def && def->getDefault() == Constant::emptyObject()) {
                return std::make_unique<ProjSpecBuilder>(NonObjInputBehavior::kNewObj,
                                                         FieldListScope::kOpen);
            }
        }

        if (p1 && p2) {
            if (p1->absorb(std::move(p2))) {
                return p1;
            } else {
                // We need to bail out and lower the entire composition to a LambdaArg, hoping that
                // higher up in the path, this lambda can be used in a MakeObjSpec (or eliminated).
                return makeLambdaBuilder(n);
            }
        }

        return nullptr;
    }

    std::unique_ptr<ProjSpecBuilder> transport(const ABT&,
                                               const EvalPath&,
                                               std::unique_ptr<ProjSpecBuilder> path,
                                               std::unique_ptr<ProjSpecBuilder>) {
        return path;
    }

    ABTVector generateMakeObjArgs(const ABT& n) {
        if (!n.is<EvalPath>()) {
            return {};
        }

        // This step may fail; if so, we will not have altered the input ABT.
        auto args = algebra::transport<true>(n, *this);
        if (!args->isValid() || args->isTrivial()) {
            // We may also fallback to lowering if we drop a trivial makeObjSpec (equivalent to a
            // PathId).
            return {};
        }

        // We were able to construct a MakeObjSpec; we can go ahead with lowering to a makeObj
        // function call. To do this, we need to populate the arguments to the function.
        ABTVector argStack;
        auto* makeObjSpec = args->build(argStack).release();

        // The original input to the EvalPath will be the second argument, corresponding to the
        // "root" document.
        argStack.insert(argStack.begin(), n.cast<EvalPath>()->getInput());

        // The generated MakeObjSpec* will be the first argument.
        argStack.insert(argStack.begin(),
                        make<Constant>(sbe::value::TypeTags::makeObjSpec,
                                       sbe::value::bitcastFrom<sbe::MakeObjSpec*>(makeObjSpec)));

        return argStack;
    }
};
}  // namespace

ABTVector generateMakeObjArgs(const ABT& arg) {
    ProjSpecBuilderTransport t;
    return t.generateMakeObjArgs(arg);
}
}  // namespace mongo::optimizer
