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
#include "mongo/util/string_map.h"

namespace mongo {

/// The state used as input and working space for Expressions.
class Variables {
    MONGO_DISALLOW_COPYING(Variables);

public:
    /**
     * Each unique variable is assigned a unique id of this type. Negative ids are reserved for
     * system variables and non-negative ids are allocated for user variables.
     */
    typedef int64_t Id;

    /**
     * Constructs a placeholder for expressions that use no variables (even builtins like ROOT or
     * REMOVE).
     */
    Variables() : _numVars(0) {}

    explicit Variables(size_t numVars, const Document& root = Document())
        : _root(root), _rest(numVars == 0 ? NULL : new Value[numVars]), _numVars(numVars) {}

    static void uassertValidNameForUserWrite(StringData varName);
    static void uassertValidNameForUserRead(StringData varName);

    // Ids for builtin variables.
    static constexpr Id kRootId = Id(-1);
    static constexpr Id kRemoveId = Id(-2);

    // Map from builtin var name to reserved id number.
    static const StringMap<Id> kBuiltinVarNameToId;

    /**
     * Use this instead of setValue for setting ROOT
     */
    void setRoot(const Document& root) {
        _root = root;
    }
    void clearRoot() {
        _root = Document();
    }
    const Document& getRoot() const {
        return _root;
    }

    /**
     * Sets the value of a user-defined variable. Illegal to use with the reserved builtin variables
     * defined above.
     */
    void setValue(Id id, const Value& value);

    /**
     * Gets the value of a user-defined or system variable.
     */
    Value getValue(Id id) const;

    /**
     * Returns Document() for non-document values, but otherwise identical to getValue().
     */
    Document getDocument(Id id) const;

private:
    Document _root;
    const std::unique_ptr<Value[]> _rest;
    const size_t _numVars;
};

/**
 * Generates Variables::Ids and keeps track of the number of Ids handed out.
 */
class VariablesIdGenerator {
public:
    VariablesIdGenerator() : _nextId(0) {}

    Variables::Id generateId() {
        return _nextId++;
    }

    /**
     * Returns the number of Ids handed out by this Generator.
     * Return value is intended to be passed to Variables constructor.
     */
    Variables::Id getIdCount() const {
        return _nextId;
    }

private:
    Variables::Id _nextId;
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
    explicit VariablesParseState(VariablesIdGenerator* idGenerator) : _idGenerator(idGenerator) {}

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
    StringMap<Variables::Id> _variables;
    VariablesIdGenerator* _idGenerator;
};

}  // namespace mongo
