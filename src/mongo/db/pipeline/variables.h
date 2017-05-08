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

#pragma once

#include "mongo/db/pipeline/document.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/string_map.h"

namespace mongo {

/// The state used as input and working space for Expressions.
class Variables {
    MONGO_DISALLOW_COPYING(Variables);

public:
    // Each unique variable is assigned a unique id of this type. Negative ids are reserved for
    // system variables and non-negative ids are allocated for user variables.
    using Id = int64_t;

    /**
     * Generates Variables::Id and keeps track of the number of Ids handed out.
     */
    class IdGenerator {
    public:
        IdGenerator() : _nextId(0) {}

        Variables::Id generateId() {
            return _nextId++;
        }

    private:
        Variables::Id _nextId;
    };

    Variables() = default;

    static void uassertValidNameForUserWrite(StringData varName);
    static void uassertValidNameForUserRead(StringData varName);

    // Ids for builtin variables.
    static constexpr Variables::Id kRootId = Id(-1);
    static constexpr Variables::Id kRemoveId = Id(-2);

    // Map from builtin var name to reserved id number.
    static const StringMap<Id> kBuiltinVarNameToId;

    /**
     * Sets the value of a user-defined variable. Illegal to use with the reserved builtin variables
     * defined above.
     */
    void setValue(Variables::Id id, const Value& value);

    /**
     * Gets the value of a user-defined or system variable. If the 'id' provided represents the
     * special ROOT variable, then we return 'root' in Value form.
     */
    Value getValue(Variables::Id id, const Document& root) const;

    /**
     * Returns Document() for non-document values, but otherwise identical to getValue(). If the
     * 'id' provided represents the special ROOT variable, then we return 'root'.
     */
    Document getDocument(Variables::Id id, const Document& root) const;

    IdGenerator* useIdGenerator() {
        return &_idGenerator;
    }


private:
    IdGenerator _idGenerator;
    std::vector<Value> _valueList;
};

/**
 * This class represents the Variables that are defined in an Expression tree.
 *
 * All copies from a given instance share enough information to ensure unique Ids are assigned
 * and to propagate back to the original instance enough information to correctly construct a
 * Variables instance.
 */
class VariablesParseState {
public:
    explicit VariablesParseState(Variables::IdGenerator* variableIdGenerator)
        : _idGenerator(variableIdGenerator) {}

    /**
     * Assigns a named variable a unique Id. This differs from all other variables, even
     * others with the same name.
     *
     * The special variables ROOT and CURRENT are always implicitly defined with CURRENT
     * equivalent to ROOT. If CURRENT is explicitly defined by a call to this function, it
     * breaks that equivalence.
     *
     * NOTE: Name validation is responsibility of caller.
     */
    Variables::Id defineVariable(StringData name);

    /**
     * Returns the current Id for a variable. uasserts if the variable isn't defined.
     */
    Variables::Id getVariable(StringData name) const;

private:
    // Not owned here.
    Variables::IdGenerator* _idGenerator;

    StringMap<Variables::Id> _variables;
};

}  // namespace mongo
