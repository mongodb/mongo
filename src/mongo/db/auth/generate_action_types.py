#!/usr/bin/env python

#    Copyright 2012 10gen Inc.
#
#    This program is free software: you can redistribute it and/or  modify
#    it under the terms of the GNU Affero General Public License, version 3,
#    as published by the Free Software Foundation.
#
#    This program is distributed in the hope that it will be useful,
#    but WITHOUT ANY WARRANTY; without even the implied warranty of
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#    GNU Affero General Public License for more details.
#
#    You should have received a copy of the GNU Affero General Public License
#    along with this program.  If not, see <http://www.gnu.org/licenses/>.
#
#    As a special exception, the copyright holders give permission to link the
#    code of portions of this program with the OpenSSL library under certain
#    conditions as described in each individual source file and distribute
#    linked combinations including the program with the OpenSSL library. You
#    must comply with the GNU Affero General Public License in all respects
#    for all of the code used other than as permitted herein. If you modify
#    file(s) with this exception, you may extend this exception to your
#    version of the file(s), but you are not obligated to do so. If you do not
#    wish to do so, delete this exception statement from your version. If you
#    delete this exception statement from all source files in the program,
#    then also delete it in the license file.

"""Generate action_type.{h,cpp}

Usage:
    python generate_action_types.py <path to action_types.txt> <header file path> <source file path>
"""

import sys


headerFileTemplate = """// AUTO-GENERATED FILE DO NOT EDIT
// See src/mongo/db/auth/generate_action_types.py
/*    Copyright 2014 MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <cstdint>
#include <iosfwd>
#include <map>
#include <string>

#include "mongo/base/status.h"

namespace mongo {

    struct ActionType {
    public:

        explicit ActionType(uint32_t identifier) : _identifier(identifier) {};
        ActionType() {};

        uint32_t getIdentifier() const {
            return _identifier;
        }

        bool operator==(const ActionType& rhs) const;

        std::string toString() const;

        // Takes the string representation of a single action type and returns the corresponding
        // ActionType enum.
        static Status parseActionFromString(const std::string& actionString, ActionType* result);

        // Takes an ActionType and returns the string representation
        static std::string actionToString(const ActionType& action);

%(actionTypeConstants)s
        enum ActionTypeIdentifier {
%(actionTypeIdentifiers)s
            actionTypeEndValue, // Should always be last in this enum
        };

        static const int NUM_ACTION_TYPES = actionTypeEndValue;

    private:

        uint32_t _identifier; // unique identifier for this action.
    };

    // String stream operator for ActionType
    std::ostream& operator<<(std::ostream& os, const ActionType& at);

} // namespace mongo
"""

sourceFileTemplate = """// AUTO-GENERATED FILE DO NOT EDIT
// See src/mongo/db/auth/generate_action_types.py
/*    Copyright 2014 MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/auth/action_type.h"

#include <cstdint>
#include <iostream>
#include <string>

#include "mongo/base/status.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

%(actionTypeConstants)s
    bool ActionType::operator==(const ActionType& rhs) const {
        return _identifier == rhs._identifier;
    }

    std::ostream& operator<<(std::ostream& os, const ActionType& at) {
        os << ActionType::actionToString(at);
        return os;
    }

    std::string ActionType::toString() const {
        return actionToString(*this);
    }

    Status ActionType::parseActionFromString(const std::string& action, ActionType* result) {
%(fromStringIfStatements)s
        return Status(ErrorCodes::FailedToParse,
                      mongoutils::str::stream() << "Unrecognized action privilege string: "
                                                << action,
                      0);
    }

    // Takes an ActionType and returns the string representation
    std::string ActionType::actionToString(const ActionType& action) {
        switch (action.getIdentifier()) {
%(toStringCaseStatements)s        default:
            return "";
        }
    }

} // namespace mongo
"""

def writeSourceFile(actionTypes, sourceOutputFile):
    actionTypeConstants = ""
    fromStringIfStatements = ""
    toStringCaseStatements = ""
    for actionType in actionTypes:
        actionTypeConstants += ("    const ActionType ActionType::%(actionType)s"
                                "(%(actionType)sValue);\n" %
                                dict(actionType=actionType))
        fromStringIfStatements += """        if (action == "%(actionType)s") {
            *result = %(actionType)s;
            return Status::OK();
        }\n""" % dict(actionType=actionType)
        toStringCaseStatements += """        case %(actionType)sValue:
            return "%(actionType)s";\n""" % dict(actionType=actionType)
    formattedSourceFile = sourceFileTemplate % dict(actionTypeConstants=actionTypeConstants,
                                                    fromStringIfStatements=fromStringIfStatements,
                                                    toStringCaseStatements=toStringCaseStatements)
    sourceOutputFile.write(formattedSourceFile)

    pass

def writeHeaderFile(actionTypes, headerOutputFile):
    actionTypeConstants = ""
    actionTypeIdentifiers = ""
    for actionType in actionTypes:
        actionTypeConstants += "        static const ActionType %s;\n" % (actionType)
        actionTypeIdentifiers += "            %sValue,\n" % (actionType)
    formattedHeaderFile = headerFileTemplate % dict(actionTypeConstants=actionTypeConstants,
                                                    actionTypeIdentifiers=actionTypeIdentifiers)
    headerOutputFile.write(formattedHeaderFile)

def hasDuplicateActionTypes(actionTypes):
    sortedActionTypes = sorted(actionTypes)

    didFail = False
    prevActionType = sortedActionTypes[0]
    for actionType in sortedActionTypes[1:]:
        if actionType == prevActionType:
            print 'Duplicate actionType %s\n' % actionType
            didFail = True
        prevActionType = actionType

    return didFail

def parseActionTypesFromFile(actionTypesFilename):
    actionTypesFile = open(actionTypesFilename, 'r')
    actionTypes = eval(actionTypesFile.read())
    return actionTypes

if __name__ == "__main__":
    if len(sys.argv) != 4:
        print "Usage: generate_action_types.py <path to action_types.txt> <header file path> <source file path>"
        sys.exit(-1)

    actionTypes = parseActionTypesFromFile(sys.argv[1])
    if hasDuplicateActionTypes(actionTypes):
        sys.exit(-1)

    headerOutputFile = open(sys.argv[2], 'w')
    sourceOutputFile = open(sys.argv[3], 'w')

    writeHeaderFile(actionTypes, headerOutputFile)
    writeSourceFile(actionTypes, sourceOutputFile)
