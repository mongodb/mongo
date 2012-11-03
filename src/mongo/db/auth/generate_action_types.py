#!/usr/bin/python

#    Copyright 2012 10gen Inc.
#
#    Licensed under the Apache License, Version 2.0 (the "License");
#    you may not use this file except in compliance with the License.
#    You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
#    Unless required by applicable law or agreed to in writing, software
#    distributed under the License is distributed on an "AS IS" BASIS,
#    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#    See the License for the specific language governing permissions and
#    limitations under the License.
#

"""Generate action_type.{h,cpp}

Usage:
    python generate_action_types.py <header file path> <source file path>
"""

import sys

# List describing the ActionTypes that should be created.
actionTypes = ["addShard",
               "applyOps",
               "captrunc",
               "clean",
               "closeAllDatabases",
               "collMod",
               "collStats",
               "compact",
               "connPoolStats",
               "connPoolSync",
               "convertToCapped",
               "cpuProfiler",
               "createCollection",
               "cursorInfo",
               "dbHash",
               "dbStats",
               "diagLogging",
               "dropCollection",
               "dropDatabase",
               "dropIndexes",
               "emptycapped",
               "enableSharding",
               "ensureIndex",
               "find",
               "flushRouterConfig",
               "fsync",
               "getCmdLineOpts",
               "getLog",
               "getParameter",
               "getShardMap",
               "getShardVersion",
               "handshake",
               "hostInfo",
               "insert",
               "listDatabases",
               "listShards",
               "logRotate",
               "moveChunk",
               "movePrimary",
               "netstat",
               "profile",
               "reIndex",
               "remove",
               "removeShard",
               "renameCollection",
               "repairDatabase",
               "replSetElect",
               "replSetFreeze",
               "replSetFresh",
               "replSetGetRBID",
               "replSetGetStatus",
               "replSetHeartbeat",
               "replSetInitiate",
               "replSetMaintenance",
               "replSetReconfig",
               "replSetStepDown",
               "replSetSyncFrom",
               "resync",
               "setParameter",
               "setShardVersion",
               "shardCollection",
               "shardingState",
               "shutdown",
               "split",
               "splitChunk",
               "splitVector",
               "top",
               "touch",
               "unsetSharding",
               "update",
               "userAdmin",
               "validate",
               "writebacklisten",
               "writeBacksQueued",
               "_migrateClone",
               "_recvChunkAbort",
               "_recvChunkCommit",
               "_recvChunkStart",
               "_recvChunkStatus",
               "_transferMods",
               "oldRead", # Temporary. For easing AuthorizationManager integration
               "oldWrite"] # Temporary. For easing AuthorizationManager integration


headerFileTemplate = """// AUTO-GENERATED FILE DO NOT EDIT
// See src/mongo/db/auth/generate_action_types.py
/*
*    Copyright (C) 2012 10gen Inc.
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
*/

#pragma once

#include <iosfwd>
#include <map>
#include <string>

#include "mongo/base/status.h"
#include "mongo/platform/cstdint.h"

namespace mongo {

    struct ActionType {
    public:

        explicit ActionType(uint32_t identifier) : _identifier(identifier) {};
        ActionType() {};

        uint32_t getIdentifier() const {
            return _identifier;
        }

        bool operator==(const ActionType& rhs) const;

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
/*
*    Copyright (C) 2012 10gen Inc.
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
*/

#include "mongo/pch.h"

#include "mongo/db/auth/action_type.h"

#include <iostream>
#include <string>

#include "mongo/base/status.h"
#include "mongo/platform/cstdint.h"
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

    Status ActionType::parseActionFromString(const std::string& action, ActionType* result) {
%(fromStringIfStatements)s
        return Status(ErrorCodes::FailedToParse,
                      mongoutils::str::stream() << "Unrecognized action capability string: "
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

def writeSourceFile(sourceOutputFile):
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

def writeHeaderFile(headerOutputFile):
    actionTypeConstants = ""
    actionTypeIdentifiers = ""
    for actionType in actionTypes:
        actionTypeConstants += "        static const ActionType %s;\n" % (actionType)
        actionTypeIdentifiers += "            %sValue,\n" % (actionType)
    formattedHeaderFile = headerFileTemplate % dict(actionTypeConstants=actionTypeConstants,
                                                    actionTypeIdentifiers=actionTypeIdentifiers)
    headerOutputFile.write(formattedHeaderFile)

def hasDuplicateActionTypes():
    sortedActionTypes = sorted(actionTypes)

    didFail = False
    prevActionType = sortedActionTypes[0]
    for actionType in sortedActionTypes[1:]:
        if actionType == prevActionType:
            print 'Duplicate actionType %s\n' % actionType
            didFail = True
        prevActionType = actionType

    return didFail

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print "Usage: generate_action_types.py <header file path> <source file path>"
        sys.exit(-1)

    if hasDuplicateActionTypes():
        sys.exit(-1)

    headerOutputFile = open(sys.argv[1], 'w')
    sourceOutputFile = open(sys.argv[2], 'w')

    writeHeaderFile(headerOutputFile)
    writeSourceFile(sourceOutputFile)
