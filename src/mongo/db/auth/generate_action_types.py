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

# List of tuples describing the ActionTypes that should be created.
# The first value in the tuple is the name of the enum, the second is the string
# representation.
actionTypes = [("ADD_SHARD", "addShard"),
               ("APPLY_OPS", "applyOps"),
               ("CAPTRUNC", "captrunc"),
               ("CLEAN", "clean"),
               ("CLOSE_ALL_DATABASES", "closeAllDatabases"),
               ("COLL_MOD", "collMod"),
               ("COLL_STATS", "collStats"),
               ("COMPACT", "compact"),
               ("CONN_POOL_STATS", "connPoolStats"),
               ("CONN_POOL_SYNC", "connPoolSync"),
               ("CONVERT_TO_CAPPED", "convertToCapped"),
               ("CPU_PROFILER", "cpuProfiler"),
               ("CREATE_COLLECTION", "createCollection"),
               ("CURSOR_INFO", "cursorInfo"),
               ("DB_HASH", "dbHash"),
               ("DB_STATS", "dbStats"),
               ("DELETE", "delete"),
               ("DIAG_LOGGING", "diagLogging"),
               ("DROP_COLLECTION", "dropCollection"),
               ("DROP_DATABASE", "dropDatabase"),
               ("DROP_INDEXES", "dropIndexes"),
               ("EMPTYCAPPED", "emptycapped"),
               ("ENABLE_SHARDING", "enableSharding"),
               ("ENSURE_INDEX", "ensureIndex"),
               ("FIND", "find"),
               ("FLUSH_ROUTER_CONFIG", "flushRouterConfig"),
               ("FSYNC", "fsync"),
               ("GET_CMD_LINE_OPTS", "getCmdLineOpts"),
               ("GET_LOG", "getLog"),
               ("GET_PARAMETER", "getParameter"),
               ("GET_SHARD_MAP", "getShardMap"),
               ("GET_SHARD_VERSION", "getShardVersion"),
               ("HANDSHAKE", "handshake"),
               ("HOST_INFO", "hostInfo"),
               ("INSERT", "insert"),
               ("LIST_DATABASES", "listDatabases"),
               ("LIST_SHARDS", "listShards"),
               ("LOG_ROTATE", "logRotate"),
               ("MOVE_CHUNK", "moveChunk"),
               ("MOVE_PRIMARY", "movePrimary"),
               ("NETSTAT", "netstat"),
               ("PROFILE", "profile"),
               ("RE_INDEX", "reIndex"),
               ("REMOVE_SHARD", "removeShard"),
               ("RENAME_COLLECTION", "renameCollection"),
               ("REPAIR_DATABASE", "repairDatabase"),
               ("REPL_SET_ELECT", "replSetElect"),
               ("REPL_SET_FREEZE", "replSetFreeze"),
               ("REPL_SET_FRESH", "replSetFresh"),
               ("REPL_SET_GET_RBID", "replSetGetRBID"),
               ("REPL_SET_GET_STATUS", "replSetGetStatus"),
               ("REPL_SET_HEARTBEAT", "replSetHeartbeat"),
               ("REPL_SET_INITIATE", "replSetInitiate"),
               ("REPL_SET_MAINTENANCE", "replSetMaintenance"),
               ("REPL_SET_RECONFIG", "replSetReconfig"),
               ("REPL_SET_STEP_DOWN", "replSetStepDown"),
               ("REPL_SET_SYNC_FROM", "replSetSyncFrom"),
               ("RESYNC", "resync"),
               ("SET_PARAMETER", "setParameter"),
               ("SET_SHARD_VERSION", "setShardVersion"),
               ("SHARD_COLLECTION", "shardCollection"),
               ("SHARDING_STATE", "shardingState"),
               ("SHUTDOWN", "shutdown"),
               ("SPLIT", "split"),
               ("SPLIT_CHUNK", "splitChunk"),
               ("SPLIT_VECTOR", "splitVector"),
               ("TOP", "top"),
               ("TOUCH", "touch"),
               ("UNSET_SHARDING", "unsetSharding"),
               ("UPDATE", "update"),
               ("USER_ADMIN", "userAdmin"),
               ("VALIDATE", "validate"),
               ("WRITEBACKLISTEN", "writebacklisten"),
               ("WRITE_BACKS_QUEUED", "writeBacksQueued"),
               ("_MIGRATE_CLONE", "_migrateClone"),
               ("_RECV_CHUNK_ABORT", "_recvChunkAbort"),
               ("_RECV_CHUNK_COMMIT", "_recvChunkCommit"),
               ("_RECV_CHUNK_START", "_recvChunkStart"),
               ("_RECV_CHUNK_STATUS", "_recvChunkStatus"),
               ("_TRANSFER_MODS", "_transferMods"),
               ("OLD_READ", "oldRead"), # Temporary. For easing AuthorizationManager integration
               ("OLD_WRITE", "oldWrite")] # Temporary. For easing AuthorizationManager integration


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
            ACTION_TYPE_END_VALUE, // Should always be last in this enum
        };

        static const int NUM_ACTION_TYPES = ACTION_TYPE_END_VALUE;

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
    for (actionType, stringRepresentation) in actionTypes:
        actionTypeConstants += ("    const ActionType ActionType::%(actionType)s"
                                "(%(actionType)s_VALUE);\n" %
                                dict(actionType=actionType))
        fromStringIfStatements += """        if (action == "%(stringRepresentation)s") {
            *result = %(actionType)s;
            return Status::OK();
        }\n""" % dict(stringRepresentation=stringRepresentation, actionType=actionType)
        toStringCaseStatements += """        case %(actionType)s_VALUE:
            return "%(stringRepresentation)s";\n""" % dict(actionType=actionType,
                                                           stringRepresentation=stringRepresentation)
    formattedSourceFile = sourceFileTemplate % dict(actionTypeConstants=actionTypeConstants,
                                                    fromStringIfStatements=fromStringIfStatements,
                                                    toStringCaseStatements=toStringCaseStatements)
    sourceOutputFile.write(formattedSourceFile)

    pass

def writeHeaderFile(headerOutputFile):
    actionTypeConstants = ""
    actionTypeIdentifiers = ""
    for (actionType, unused) in actionTypes:
        actionTypeConstants += "        static const ActionType %s;\n" % (actionType)
        actionTypeIdentifiers += "            %s_VALUE,\n" % (actionType)
    formattedHeaderFile = headerFileTemplate % dict(actionTypeConstants=actionTypeConstants,
                                                    actionTypeIdentifiers=actionTypeIdentifiers)
    headerOutputFile.write(formattedHeaderFile)

def hasDuplicateActionTypes():
    sorted_by_name = sorted(actionTypes, key=lambda x: x[0])
    sorted_by_string = sorted(actionTypes, key=lambda x: x[1])

    didFail = False
    prev_name, prev_string = sorted_by_name[0]
    for name, string in sorted_by_name[1:]:
        if name == prev_name:
            print 'Duplicate name %s with string descriptions %s and %s\n' % (name, string, prev_string)
            didFail = True
        prev_name, prev_string = name, string

    prev_name, prev_string = sorted_by_string[0]
    for name, string in sorted_by_string[1:]:
        if string == prev_string:
            print 'Duplicate string description %s for actions %s and %s\n' % (string, name, prev_name)
            didFail = True
        prev_name, prev_string = name, string

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
