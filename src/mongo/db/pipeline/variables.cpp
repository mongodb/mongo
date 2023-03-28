/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/pipeline/variables.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_dependencies.h"
#include "mongo/db/pipeline/variable_validation.h"
#include "mongo/db/vector_clock.h"
#include "mongo/platform/basic.h"
#include "mongo/platform/random.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

namespace mongo {

using namespace std::string_literals;

constexpr Variables::Id Variables::kRootId;
constexpr Variables::Id Variables::kRemoveId;

constexpr StringData kRootName = "ROOT"_sd;
constexpr StringData kRemoveName = "REMOVE"_sd;
constexpr StringData kNowName = "NOW"_sd;
constexpr StringData kClusterTimeName = "CLUSTER_TIME"_sd;
constexpr StringData kJsScopeName = "JS_SCOPE"_sd;
constexpr StringData kIsMapReduceName = "IS_MR"_sd;
constexpr StringData kSearchMetaName = "SEARCH_META"_sd;
constexpr StringData kUserRolesName = "USER_ROLES"_sd;

const StringMap<Variables::Id> Variables::kBuiltinVarNameToId = {
    {kRootName.rawData(), kRootId},
    {kRemoveName.rawData(), kRemoveId},
    {kNowName.rawData(), kNowId},
    {kClusterTimeName.rawData(), kClusterTimeId},
    {kJsScopeName.rawData(), kJsScopeId},
    {kIsMapReduceName.rawData(), kIsMapReduceId},
    {kSearchMetaName.rawData(), kSearchMetaId},
    {kUserRolesName.rawData(), kUserRolesId}};

const std::map<Variables::Id, std::string> Variables::kIdToBuiltinVarName = {
    {kRootId, kRootName.rawData()},
    {kRemoveId, kRemoveName.rawData()},
    {kNowId, kNowName.rawData()},
    {kClusterTimeId, kClusterTimeName.rawData()},
    {kJsScopeId, kJsScopeName.rawData()},
    {kIsMapReduceId, kIsMapReduceName.rawData()},
    {kSearchMetaId, kSearchMetaName.rawData()},
    {kUserRolesId, kUserRolesName.rawData()}};

const std::map<StringData, std::function<void(const Value&)>> Variables::kSystemVarValidators = {
    {kNowName,
     [](const auto& value) {
         uassert(ErrorCodes::TypeMismatch,
                 str::stream() << "$$NOW must have a date value, found "
                               << typeName(value.getType()),
                 value.getType() == BSONType::Date);
     }},
    {kClusterTimeName,
     [](const auto& value) {
         uassert(ErrorCodes::TypeMismatch,
                 str::stream() << "$$CLUSTER_TIME must have a timestamp value, found "
                               << typeName(value.getType()),
                 value.getType() == BSONType::bsonTimestamp);
     }},
    {kJsScopeName,
     [](const auto& value) {
         uassert(ErrorCodes::TypeMismatch,
                 str::stream() << "$$JS_SCOPE must have an object value, found "
                               << typeName(value.getType()),
                 value.getType() == BSONType::Object);
     }},
    {kIsMapReduceName,
     [](const auto& value) {
         uassert(ErrorCodes::TypeMismatch,
                 str::stream() << "$$IS_MR must have a bool value, found "
                               << typeName(value.getType()),
                 value.getType() == BSONType::Bool);
     }},
    {kUserRolesName, [](const auto& value) {
         uassert(ErrorCodes::TypeMismatch,
                 str::stream() << "$$USER_ROLES must have an array value, found "
                               << typeName(value.getType()),
                 value.getType() == BSONType::Array);
     }}};

void Variables::setValue(Id id, const Value& value, bool isConstant) {
    uassert(17199, "can't use Variables::setValue to set a reserved builtin variable", id >= 0);

    // If a value has already been set for 'id', and that value was marked as constant, then it
    // is illegal to modify.
    invariant(!hasConstantValue(id));
    _definitions[id] = {value, isConstant};
}

void Variables::setReservedValue(Id id, const Value& value, bool isConstant) {
    // If a value has already been set for 'id', and that value was marked as constant, then it
    // is illegal to modify.
    switch (id) {
        case Variables::kSearchMetaId:
            tassert(5858101,
                    "Can't set a variable that has been set to be constant ",
                    !hasConstantValue(id));
            _definitions[id] = {value, isConstant};
            break;
        default:
            // Currently it is only allowed to manually set the SEARCH_META builtin variable.
            tasserted(5858102,
                      str::stream() << "Attempted to set '$$" << getBuiltinVariableName(id)
                                    << "' which is not permitted");
    }
}

void Variables::setValue(Variables::Id id, const Value& value) {
    const bool isConstant = false;
    setValue(id, value, isConstant);
}

void Variables::setConstantValue(Variables::Id id, const Value& value) {
    const bool isConstant = true;
    setValue(id, value, isConstant);
}

Value Variables::getUserDefinedValue(Variables::Id id) const {
    invariant(isUserDefinedVariable(id));

    auto it = _definitions.find(id);
    uassert(40434, str::stream() << "Undefined variable id: " << id, it != _definitions.end());
    return it->second.value;
}

Value Variables::getValue(Id id, const Document& root) const {
    if (id < 0) {
        // This is a reserved id for a builtin variable.
        switch (id) {
            case Variables::kRootId:
                return Value(root);
            case Variables::kRemoveId:
                return Value();
            case Variables::kNowId:
            case Variables::kClusterTimeId:
            case Variables::kJsScopeId:
            case Variables::kIsMapReduceId:
            case Variables::kUserRolesId:
                if (auto it = _definitions.find(id); it != _definitions.end()) {
                    return it->second.value;
                }
                uasserted(51144,
                          str::stream() << "Builtin variable '$$" << getBuiltinVariableName(id)
                                        << "' is not available");
            case Variables::kSearchMetaId: {
                auto metaIt = _definitions.find(id);
                return metaIt == _definitions.end() ? Value() : metaIt->second.value;
            }
            default:
                MONGO_UNREACHABLE;
        }
    }

    return getUserDefinedValue(id);
}

Document Variables::getDocument(Id id, const Document& root) const {
    if (id == Variables::kRootId) {
        // For the common case of ROOT, avoid round-tripping through Value.
        return root;
    }

    const Value var = getValue(id, root);
    if (var.getType() == Object)
        return var.getDocument();

    return Document();
}

void Variables::setLegacyRuntimeConstants(const LegacyRuntimeConstants& constants) {
    const bool constant = true;
    _definitions[kNowId] = {Value(constants.getLocalNow()), constant};
    // We use a null Timestamp to indicate that the clusterTime is not available; this can happen if
    // the logical clock is not running. We do not use boost::optional because this would allow the
    // IDL to serialize a RuntimConstants without clusterTime, which should always be an error.
    if (!constants.getClusterTime().isNull()) {
        _definitions[kClusterTimeId] = {Value(constants.getClusterTime()), constant};
    }

    if (constants.getJsScope()) {
        _definitions[kJsScopeId] = {Value(*constants.getJsScope()), constant};
    }
    if (constants.getIsMapReduce()) {
        _definitions[kIsMapReduceId] = {Value(*constants.getIsMapReduce()), constant};
    }
    if (constants.getUserRoles()) {
        _definitions[kUserRolesId] = {Value(constants.getUserRoles().value()), constant};
    }
}

void Variables::setDefaultRuntimeConstants(OperationContext* opCtx) {
    setLegacyRuntimeConstants(Variables::generateRuntimeConstants(opCtx));
}

void Variables::appendSystemVariables(BSONObjBuilder& bob) const {
    for (auto&& [name, id] : kBuiltinVarNameToId) {
        if (hasValue(id)) {
            // We should serialize the system variables using $literal (as we do with the
            // user-defined variables) so that they get parsed the same way (for example, not using
            // $literal to parse a variable value that is an array causes the expression context to
            // be marked as SBE incompatible through ExpressionArray).
            bob << name << Value(DOC("$literal" << getValue(id)));
        }
    }
}

namespace {

/**
 * Returns a callback function which can be used to verify the value conforms to expectations if
 * 'varName' is a reserved system variable. Throws an exception if 'varName' is a reserved name
 * (e.g. capital letter) but not one of the known variables. Returns boost::none for normal
 * variables.
 */
boost::optional<std::function<void(const Value&)>> validateVariable(OperationContext* opCtx,
                                                                    StringData varName) {
    auto validateStatus = variableValidation::isValidNameForUserWrite(varName);
    if (validateStatus.isOK()) {
        return boost::none;
    }
    // Reserved field name. It may be an internal propogation of a constant. Otherwise we need to
    // reject it.
    const auto& knownConstantIt = Variables::kSystemVarValidators.find(varName);
    if (knownConstantIt == Variables::kSystemVarValidators.end()) {
        uassertStatusOKWithContext(validateStatus, "Invalid 'let' parameter");
    }

    uassert(4738901,
            str::stream() << "Attempt to set internal constant: " << varName,
            opCtx->getClient()->session() &&
                (opCtx->getClient()->session()->getTags() & transport::Session::kInternalClient));

    return knownConstantIt->second;
}

}  // namespace

void Variables::seedVariablesWithLetParameters(ExpressionContext* const expCtx,
                                               const BSONObj letParams) {
    for (auto&& elem : letParams) {
        const auto fieldName = elem.fieldNameStringData();
        auto maybeSystemVarValidator = validateVariable(expCtx->opCtx, fieldName);
        auto expr = Expression::parseOperand(expCtx, elem, expCtx->variablesParseState);

        uassert(4890500,
                "Command let Expression tried to access a field, but this is not allowed because"
                "Command let Expressions run before the query examines any documents.",
                expression::getDependencies(expr.get()).hasNoRequirements());
        Value value = expr->evaluate(Document{}, &expCtx->variables);

        if (maybeSystemVarValidator) {
            (*maybeSystemVarValidator)(value);
            if (!(fieldName == kClusterTimeName && value.getTimestamp().isNull())) {
                // Avoid populating a value for CLUSTER_TIME if the value is null.
                _definitions[kBuiltinVarNameToId.at(fieldName)] = {value, true};
            }
        } else {
            setConstantValue(expCtx->variablesParseState.defineVariable(fieldName), value);
        }
    }
}

LegacyRuntimeConstants Variables::generateRuntimeConstants(OperationContext* opCtx) {
    // On a standalone, the clock may not be running and $$CLUSTER_TIME is unavailable. If the
    // logical clock is available, set the clusterTime in the runtime constants. Otherwise, the
    // clusterTime is set to the null Timestamp.
    if (opCtx->getClient()) {
        if (const auto vectorClock = VectorClock::get(opCtx)) {
            const auto now = vectorClock->getTime();
            if (VectorClock::isValidComponentTime(now.clusterTime())) {
                return {Date_t::now(), now.clusterTime().asTimestamp()};
            }
        }
    }
    return {Date_t::now(), Timestamp()};
}

void Variables::copyToExpCtx(const VariablesParseState& vps, ExpressionContext* expCtx) const {
    expCtx->variables = *this;
    expCtx->variablesParseState = vps.copyWith(expCtx->variables.useIdGenerator());
}

LegacyRuntimeConstants Variables::transitionalExtractRuntimeConstants() const {
    LegacyRuntimeConstants extracted;
    for (auto&& [builtinName, ignoredValidator] : kSystemVarValidators) {
        const auto builtinId = kBuiltinVarNameToId.at(builtinName);
        if (auto it = _definitions.find(builtinId); it != _definitions.end()) {
            const auto& [value, unusedIsConstant] = it->second;
            switch (builtinId) {
                case kNowId: {
                    invariant(value.getType() == BSONType::Date);
                    extracted.setLocalNow(value.getDate());
                    break;
                }
                case kClusterTimeId: {
                    invariant(value.getType() == BSONType::bsonTimestamp);
                    extracted.setClusterTime(value.getTimestamp());
                    break;
                }
                case kJsScopeId: {
                    invariant(value.getType() == BSONType::Object);
                    extracted.setJsScope(value.getDocument().toBson());
                    break;
                }
                case kIsMapReduceId: {
                    invariant(value.getType() == BSONType::Bool);
                    extracted.setIsMapReduce(value.getBool());
                    break;
                }
                case kUserRolesId: {
                    invariant(value.getType() == BSONType::Array);
                    BSONArrayBuilder bab;
                    for (const auto& val : value.getArray()) {
                        invariant(val.getType() == BSONType::Object);
                        bab.append(val.getDocument().toBson());
                    }
                    extracted.setUserRoles(bab.arr());
                    break;
                }
                default:
                    MONGO_UNREACHABLE;
            }
        }
    }
    return extracted;
}

void Variables::defineUserRoles(OperationContext* opCtx) {
    auto* as = AuthorizationSession::get(opCtx->getClient());

    auto roleNames =
        as->isImpersonating() ? as->getImpersonatedRoleNames() : as->getAuthenticatedRoleNames();
    // Marshall current effective user roles into an array of
    // {_id: ..., db: ..., role: ...} objects for the $$USER_ROLES variable.
    BSONArrayBuilder builder;
    for (; roleNames.more(); roleNames.next()) {
        BSONObjBuilder bob(builder.subobjStart());

        bob.append("_id"_sd, roleNames->getUnambiguousName());
        bob.append("role"_sd, roleNames->getRole());
        bob.append("db"_sd, roleNames->getDB());
        bob.doneFast();
    }

    _definitions[kUserRolesId] = {Value(builder.arr()), true /* isConst */};
}

Variables::Id VariablesParseState::defineVariable(StringData name) {
    // Caller should have validated before hand by using
    // variableValidation::validateNameForUserWrite.
    massert(17275,
            "Can't redefine a non-user-writable variable",
            Variables::kBuiltinVarNameToId.find(name) == Variables::kBuiltinVarNameToId.end());

    Variables::Id id = _idGenerator->generateId();
    invariant(id > _lastSeen);

    _variables[name] = _lastSeen = id;
    return id;
}

Variables::Id VariablesParseState::getVariable(StringData name) const {
    auto it = _variables.find(name);
    if (it != _variables.end()) {
        // Found a user-defined variable.
        return it->second;
    }

    it = Variables::kBuiltinVarNameToId.find(name);
    if (it != Variables::kBuiltinVarNameToId.end()) {
        // This is a builtin variable.
        return it->second;
    }

    // If we didn't find either a user-defined or builtin variable, then we reject everything other
    // than CURRENT. If this is CURRENT, then we treat it as equivalent to ROOT.
    uassert(17276, str::stream() << "Use of undefined variable: " << name, name == "CURRENT");
    return Variables::kRootId;
}

std::set<Variables::Id> VariablesParseState::getDefinedVariableIDs() const {
    std::set<Variables::Id> ids;

    for (auto&& keyId : _variables) {
        ids.insert(keyId.second);
    }

    return ids;
}

BSONObj VariablesParseState::serialize(const Variables& vars) const {
    auto bob = BSONObjBuilder{};
    for (auto&& [var_name, id] : _variables)
        if (vars.hasValue(id))
            bob << var_name << Value(DOC("$literal" << vars.getValue(id)));

    // System variables have to be added separately since the variable IDs are reserved and not
    // allocated like normal variables, and so not present in '_variables'.
    vars.appendSystemVariables(bob);
    return bob.obj();
}

std::pair<LegacyRuntimeConstants, BSONObj> VariablesParseState::transitionalCompatibilitySerialize(
    const Variables& vars) const {
    auto bob = BSONObjBuilder{};
    for (auto&& [var_name, id] : _variables)
        if (vars.hasValue(id))
            bob << var_name << Value(DOC("$literal" << vars.getValue(id)));

    return {vars.transitionalExtractRuntimeConstants(), bob.obj()};
}
}  // namespace mongo
