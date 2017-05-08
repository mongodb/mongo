/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/variables.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

constexpr Variables::Id Variables::kRootId;
constexpr Variables::Id Variables::kRemoveId;

const StringMap<Variables::Id> Variables::kBuiltinVarNameToId = {{"ROOT"_sd, kRootId},
                                                                 {"REMOVE"_sd, kRemoveId}};

void Variables::uassertValidNameForUserWrite(StringData varName) {
    // System variables users allowed to write to (currently just one)
    if (varName == "CURRENT") {
        return;
    }

    uassert(16866, "empty variable names are not allowed", !varName.empty());

    const bool firstCharIsValid =
        (varName[0] >= 'a' && varName[0] <= 'z') || (varName[0] & '\x80')  // non-ascii
        ;

    uassert(16867,
            str::stream() << "'" << varName
                          << "' starts with an invalid character for a user variable name",
            firstCharIsValid);

    for (size_t i = 1; i < varName.size(); i++) {
        const bool charIsValid = (varName[i] >= 'a' && varName[i] <= 'z') ||
            (varName[i] >= 'A' && varName[i] <= 'Z') || (varName[i] >= '0' && varName[i] <= '9') ||
            (varName[i] == '_') || (varName[i] & '\x80')  // non-ascii
            ;

        uassert(16868,
                str::stream() << "'" << varName << "' contains an invalid character "
                              << "for a variable name: '"
                              << varName[i]
                              << "'",
                charIsValid);
    }
}

void Variables::uassertValidNameForUserRead(StringData varName) {
    uassert(16869, "empty variable names are not allowed", !varName.empty());

    const bool firstCharIsValid = (varName[0] >= 'a' && varName[0] <= 'z') ||
        (varName[0] >= 'A' && varName[0] <= 'Z') || (varName[0] & '\x80')  // non-ascii
        ;

    uassert(16870,
            str::stream() << "'" << varName
                          << "' starts with an invalid character for a variable name",
            firstCharIsValid);

    for (size_t i = 1; i < varName.size(); i++) {
        const bool charIsValid = (varName[i] >= 'a' && varName[i] <= 'z') ||
            (varName[i] >= 'A' && varName[i] <= 'Z') || (varName[i] >= '0' && varName[i] <= '9') ||
            (varName[i] == '_') || (varName[i] & '\x80')  // non-ascii
            ;

        uassert(16871,
                str::stream() << "'" << varName << "' contains an invalid character "
                              << "for a variable name: '"
                              << varName[i]
                              << "'",
                charIsValid);
    }
}

void Variables::setValue(Id id, const Value& value) {
    uassert(17199, "can't use Variables::setValue to set a reserved builtin variable", id >= 0);

    const auto idAsSizeT = static_cast<size_t>(id);
    if (idAsSizeT >= _valueList.size()) {
        _valueList.resize(idAsSizeT + 1);
    }

    _valueList[id] = value;
}

Value Variables::getValue(Id id, const Document& root) const {
    if (id < 0) {
        // This is a reserved id for a builtin variable.
        switch (id) {
            case Variables::kRootId:
                return Value(root);
            case Variables::kRemoveId:
                return Value();
            default:
                MONGO_UNREACHABLE;
        }
    }

    uassert(40434,
            str::stream() << "Requesting Variables::getValue with an out of range id: " << id,
            static_cast<size_t>(id) < _valueList.size());
    return _valueList[id];
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

Variables::Id VariablesParseState::defineVariable(StringData name) {
    // Caller should have validated before hand by using Variables::uassertValidNameForUserWrite.
    massert(17275,
            "Can't redefine a non-user-writable variable",
            Variables::kBuiltinVarNameToId.find(name) == Variables::kBuiltinVarNameToId.end());

    Variables::Id id = _idGenerator->generateId();
    _variables[name] = id;
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
}
