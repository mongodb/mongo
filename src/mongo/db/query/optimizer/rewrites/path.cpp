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

#include "mongo/db/query/optimizer/rewrites/path.h"
#include "mongo/db/query/optimizer/utils/utils.h"

namespace mongo::optimizer {
ABT::reference_type PathFusion::follow(ABT::reference_type n) {
    if (auto var = n.cast<Variable>(); var) {
        auto def = _env.getDefinition(*var);
        if (!def.definition.empty() && !def.definition.is<Source>()) {
            return follow(def.definition);
        }
    }

    return n;
}

bool PathFusion::fuse(ABT& lhs, const ABT& rhs) {
    if (auto rhsComposeM = rhs.cast<PathComposeM>(); rhsComposeM != nullptr) {
        for (const auto& branch : collectComposed(rhs)) {
            if (_info[branch.cast<PathSyntaxSort>()]._isConst && fuse(lhs, branch)) {
                return true;
            }
        }
    }

    if (auto lhsGet = lhs.cast<PathGet>(); lhsGet != nullptr) {
        if (auto rhsField = rhs.cast<PathField>();
            rhsField != nullptr && lhsGet->name() == rhsField->name()) {
            return fuse(lhsGet->getPath(), rhsField->getPath());
        }

        if (auto rhsKeep = rhs.cast<PathKeep>(); rhsKeep != nullptr) {
            if (rhsKeep->getNames().count(lhsGet->name()) > 0) {
                return true;
            }
        }
    }

    if (auto lhsTraverse = lhs.cast<PathTraverse>(); lhsTraverse != nullptr) {
        if (auto rhsTraverse = rhs.cast<PathTraverse>();
            rhsTraverse != nullptr && lhsTraverse->getMaxDepth() == rhsTraverse->getMaxDepth()) {
            return fuse(lhsTraverse->getPath(), rhsTraverse->getPath());
        }

        auto rhsType = _info[rhs.cast<PathSyntaxSort>()]._type;
        if (rhsType != Type::unknown && rhsType != Type::array) {
            auto result = std::exchange(lhsTraverse->getPath(), make<Blackhole>());
            std::swap(lhs, result);
            return fuse(lhs, rhs);
        }
    }

    if (lhs.is<PathIdentity>()) {
        lhs = rhs;
        return true;
    }

    if (rhs.is<PathLambda>()) {
        lhs = make<PathComposeM>(std::move(lhs), rhs);
        return true;
    }

    if (auto rhsConst = rhs.cast<PathConstant>(); rhsConst != nullptr) {
        if (auto lhsCmp = lhs.cast<PathCompare>(); lhsCmp != nullptr) {
            auto result = make<PathConstant>(make<BinaryOp>(
                lhsCmp->op(),
                make<BinaryOp>(Operations::Cmp3w, rhsConst->getConstant(), lhsCmp->getVal()),
                Constant::int64(0)));

            std::swap(lhs, result);
            return true;
        }

        switch (_kindCtx.back()) {
            case Kind::filter:
                break;

            case Kind::project:
                lhs = make<PathComposeM>(rhs, std::move(lhs));
                return true;

            default:
                MONGO_UNREACHABLE;
        }
    }

    return false;
}

bool PathFusion::optimize(ABT& root) {
    for (;;) {
        _changed = false;
        algebra::transport<true>(root, *this);

        // If we have nodes in _redundant then continue iterating even if _changed is not set.
        if (!_changed && _redundant.empty()) {
            break;
        }

        _env.rebuild(root);
        _info.clear();
    }

    return false;
}

void PathFusion::transport(ABT& n, const PathConstant& path, ABT& c) {
    CollectedInfo ci;
    if (auto exprC = path.getConstant().cast<Constant>(); exprC) {
        // Let's see what we can determine from the constant expression
        auto [tag, val] = exprC->get();
        if (sbe::value::isObject(tag)) {
            ci._type = Type::object;
        } else if (sbe::value::isArray(tag)) {
            ci._type = Type::array;
        } else if (tag == sbe::value::TypeTags::Boolean) {
            ci._type = Type::boolean;
        } else if (tag == sbe::value::TypeTags::Nothing) {
            ci._type = Type::nothing;
        } else {
            ci._type = Type::any;
        }
    }

    ci._isConst = true;
    _info[&path] = ci;
}

void PathFusion::transport(ABT& n, const PathCompare& path, ABT& c) {
    CollectedInfo ci;

    // TODO - follow up on Nothing and 3 value logic. Assume plain boolean for now.
    ci._type = Type::boolean;
    ci._isConst = _info[path.getVal().cast<PathSyntaxSort>()]._isConst;
    _info[&path] = ci;
}

void PathFusion::transport(ABT& n, const PathGet& get, ABT& path) {
    if (_changed) {
        return;
    }

    // Get "a" Const <c> -> Const <c>
    if (auto constPath = path.cast<PathConstant>(); constPath) {
        // Pull out the constant path
        auto result = std::exchange(path, make<Blackhole>());

        // And swap it for the current node
        std::swap(n, result);

        _changed = true;
    } else {
        auto it = _info.find(path.cast<PathSyntaxSort>());
        uassert(6624129, "expected to find path", it != _info.end());

        // Simply move the info from the child.
        _info[&get] = it->second;
    }
}

void PathFusion::transport(ABT& n, const PathField& field, ABT& path) {
    auto it = _info.find(path.cast<PathSyntaxSort>());
    uassert(6624130, "expected to find path", it != _info.end());

    CollectedInfo ci;
    if (it->second._type == Type::unknown) {
        // We don't know anything about the child.
        ci._type = Type::unknown;
    } else if (it->second._type == Type::nothing) {
        // We are setting a field to nothing (aka drop) hence we do not know what the result could
        // be (i.e. it all depends on the input).
        ci._type = Type::unknown;
    } else {
        // The child produces bona fide value hence this will become an object.
        ci._type = Type::object;
    }

    ci._isConst = it->second._isConst;
    _info[&field] = ci;
}

void PathFusion::transport(ABT& n, const PathTraverse& path, ABT& inner) {
    // Traverse is completely dependent on its input and we cannot say anything about it.
    CollectedInfo ci;
    ci._type = Type::unknown;
    ci._isConst = false;
    _info[&path] = ci;
}

void PathFusion::transport(ABT& n, const PathComposeM& path, ABT& p1, ABT& p2) {
    if (_changed) {
        return;
    }

    if (auto p1Const = p1.cast<PathConstant>(); p1Const != nullptr) {
        switch (_kindCtx.back()) {
            case Kind::filter:
                n = make<PathConstant>(make<EvalFilter>(p2, p1Const->getConstant()));
                _changed = true;
                return;

            case Kind::project:
                n = make<PathConstant>(make<EvalPath>(p2, p1Const->getConstant()));
                _changed = true;
                return;

            default:
                MONGO_UNREACHABLE;
        }
    }

    if (auto p1Get = p1.cast<PathGet>(); p1Get != nullptr && p1Get->getPath().is<PathIdentity>()) {
        // TODO: handle chain of Gets.
        n = make<PathGet>(p1Get->name(), std::move(p2));
        _changed = true;
        return;
    }

    if (p1.is<PathIdentity>()) {
        // Id * p2 -> p2
        auto result = std::exchange(p2, make<Blackhole>());
        std::swap(n, result);
        _changed = true;
        return;
    } else if (p2.is<PathIdentity>()) {
        // p1 * Id -> p1
        auto result = std::exchange(p1, make<Blackhole>());
        std::swap(n, result);
        _changed = true;
        return;
    } else if (_redundant.erase(p1.cast<PathSyntaxSort>())) {
        auto result = std::exchange(p2, make<Blackhole>());
        std::swap(n, result);
        _changed = true;
        return;
    } else if (_redundant.erase(p2.cast<PathSyntaxSort>())) {
        auto result = std::exchange(p1, make<Blackhole>());
        std::swap(n, result);
        _changed = true;
        return;
    }

    auto p1InfoIt = _info.find(p1.cast<PathSyntaxSort>());
    auto p2InfoIt = _info.find(p2.cast<PathSyntaxSort>());

    uassert(6624131, "info must be defined", p1InfoIt != _info.end() && p2InfoIt != _info.end());

    if (p1.is<PathDefault>() && p2InfoIt->second.isNotNothing()) {
        // Default * Const e -> e (provided we can prove e is not Nothing and we can do that only
        // when e is Constant expression)
        auto result = std::exchange(p2, make<Blackhole>());
        std::swap(n, result);
        _changed = true;
    } else if (p2.is<PathDefault>() && p1InfoIt->second.isNotNothing()) {
        // Const e * Default -> e (provided we can prove e is not Nothing and we can do that only
        // when e is Constant expression)
        auto result = std::exchange(p1, make<Blackhole>());
        std::swap(n, result);
        _changed = true;
    } else if (p2InfoIt->second._type == Type::object) {
        auto left = collectComposed(p1);
        for (auto l : left) {
            if (l.is<PathObj>()) {
                _redundant.emplace(l.cast<PathSyntaxSort>());

                // Specifically not setting _changed here. Since we are trying to erase a child we
                // need to traverse the tree again on the next iteration of the optimize() loop (see
                // conditions above which erase from _redundant).
            }
        }
        _info[&path] = p2InfoIt->second;
    } else {
        _info[&path] = p2InfoIt->second;
    }
}

void PathFusion::tryFuseComposition(ABT& n, ABT& input) {
    // Check to see if our flattened composition consists of constant branches containing only
    // Field and Keep elements. If we have duplicate Field branches then retain only the
    // latest one. For example:
    //      (Field "a" ConstPath1) * (Field "b" ConstPath2) * Keep "a" -> Field "a" ConstPath1
    //      (Field "a" ConstPath1) * (Field "a" ConstPath2) -> Field "a" ConstPath2
    // TODO: handle Drop elements.

    // Latest value per field.
    opt::unordered_map<FieldNameType, ABT> fieldMap;
    // Used to preserve the relative order in which fields are set on the result.
    FieldPathType orderedFieldNames;
    boost::optional<std::set<FieldNameType>> toKeep;

    Type inputType = Type::any;
    if (auto constPtr = input.cast<Constant>(); constPtr != nullptr && constPtr->isObject()) {
        inputType = Type::object;
    }

    bool updated = false;
    bool hasDefault = false;
    for (const auto& branch : collectComposed(n)) {
        auto info = _info.find(branch.cast<PathSyntaxSort>());
        if (info == _info.cend()) {
            return;
        }

        if (auto fieldPtr = branch.cast<PathField>()) {
            if (!info->second._isConst) {
                // Rewrite is valid only with constant paths.
                return;
            }

            // Overwrite field with the latest value.
            if (fieldMap.insert_or_assign(fieldPtr->name(), branch).second) {
                orderedFieldNames.push_back(fieldPtr->name());
            } else {
                updated = true;
            }
        } else if (auto keepPtr = branch.cast<PathKeep>()) {
            for (auto it = fieldMap.begin(); it != fieldMap.cend();) {
                if (keepPtr->getNames().count(it->first) == 0) {
                    // Field is not kept, erase.
                    fieldMap.erase(it++);
                    updated = true;
                } else {
                    it++;
                }
            }

            auto newKeepSet = keepPtr->getNames();
            if (toKeep) {
                for (auto it = newKeepSet.begin(); it != newKeepSet.end();) {
                    if (toKeep->count(*it) == 0) {
                        // Field was not previously kept.
                        newKeepSet.erase(it++);
                        updated = true;
                    } else {
                        it++;
                    }
                }
            }
            toKeep = std::move(newKeepSet);
        } else if (auto defaultPtr = branch.cast<PathDefault>(); defaultPtr != nullptr) {
            if (auto constPtr = defaultPtr->getDefault().cast<Constant>();
                constPtr != nullptr && constPtr->isObject() && inputType == Type::object) {
                // Skip over PathDefault with an empty object since our input is already an object.
                updated = true;
            } else {
                // Skip over other PathDefaults but remember we had one.
                hasDefault = true;
            }
        } else {
            return;
        }
    }

    if (toKeep && input != Constant::emptyObject()) {
        // Check if we assign to every field we keep. If so, drop dependence on input.
        bool assignToAllKeptFields = true;
        for (const auto& fieldName : *toKeep) {
            if (fieldMap.count(fieldName) == 0) {
                assignToAllKeptFields = false;
                break;
            }
        }
        if (assignToAllKeptFields) {
            // Do not need the input, do not keep fields, and ignore defaults.
            input = Constant::emptyObject();
            toKeep = boost::none;
            updated = true;
            hasDefault = false;
        }
    }
    if (!updated) {
        return;
    }
    if (hasDefault) {
        return;
    }

    ABT result = make<PathIdentity>();
    if (toKeep) {
        maybeComposePath(result, make<PathKeep>(std::move(*toKeep)));
    }
    for (const auto& fieldName : orderedFieldNames) {
        auto it = fieldMap.find(fieldName);
        if (it != fieldMap.cend()) {
            // We may have removed the field name by virtue of not keeping.
            maybeComposePath(result, it->second);
        }
    }

    std::swap(n, result);
    _changed = true;
}

void PathFusion::transport(ABT& n, const EvalPath& eval, ABT& path, ABT& input) {
    if (_changed) {
        return;
    }

    auto realInput = follow(input);
    // If we are evaluating const path then we can simply replace the whole expression with the
    // result.
    if (auto constPath = path.cast<PathConstant>(); constPath) {
        // Pull out the constant out of the path
        auto result = std::exchange(constPath->getConstant(), make<Blackhole>());

        // And swap it for the current node
        std::swap(n, result);

        _changed = true;
    } else if (auto evalInput = realInput.cast<EvalPath>(); evalInput) {
        // An input to 'this' EvalPath expression is another EvalPath so we may try to fuse the
        // paths.
        if (fuse(n.cast<EvalPath>()->getPath(), evalInput->getPath())) {
            // We have fused paths so replace the input (by making a copy).
            input = evalInput->getInput();

            _changed = true;
        } else if (auto evalImmediateInput = input.cast<EvalPath>();
                   evalImmediateInput != nullptr) {
            // Compose immediate EvalPath input.
            n = make<EvalPath>(
                make<PathComposeM>(std::move(evalImmediateInput->getPath()), std::move(path)),
                std::move(evalImmediateInput->getInput()));

            _changed = true;
        }
    } else {
        tryFuseComposition(path, input);
    }

    _kindCtx.pop_back();
}

void PathFusion::transport(ABT& n, const EvalFilter& eval, ABT& path, ABT& input) {
    if (_changed) {
        return;
    }

    auto realInput = follow(input);
    // If we are evaluating const path then we can simply replace the whole expression with the
    // result.
    if (auto constPath = path.cast<PathConstant>(); constPath) {
        // Pull out the constant out of the path
        auto result = std::exchange(constPath->getConstant(), make<Blackhole>());

        // And swap it for the current node
        std::swap(n, result);

        _changed = true;
    } else if (auto evalImmediateInput = input.cast<EvalFilter>(); evalImmediateInput != nullptr) {
        // Compose immediate EvalFilter input.
        n = make<EvalFilter>(
            make<PathComposeM>(std::move(evalImmediateInput->getPath()), std::move(path)),
            std::move(evalImmediateInput->getInput()));

        _changed = true;
    } else if (auto evalInput = realInput.cast<EvalPath>(); evalInput) {
        // An input to 'this' EvalFilter expression is another EvalPath so we may try to fuse the
        // paths.
        if (fuse(n.cast<EvalFilter>()->getPath(), evalInput->getPath())) {
            // We have fused paths so replace the input (by making a copy).
            input = evalInput->getInput();

            _changed = true;
        }
    } else {
        tryFuseComposition(path, input);
    }

    _kindCtx.pop_back();
}
}  // namespace mongo::optimizer
