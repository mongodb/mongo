// strategy.h
/*
 *    Copyright (C) 2010 10gen Inc.
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

#include "mongo/pch.h"
#include "chunk.h"
#include "request.h"

namespace mongo {

    /**
     * Legacy interface for processing client read/write/cmd requests.
     */
    class Strategy {
    public:

        Strategy() {}

        void queryOp( Request& r );

        void getMore( Request& r );

        void writeOp( int op , Request& r );

        struct CommandResult {
            Shard shardTarget;
            ConnectionString target;
            BSONObj result;
        };

        /**
         * Executes a command against a particular database, and targets the command based on a
         * collection in that database.
         *
         * This version should be used by internal commands when possible.
         */
        void commandOp( const string& db,
                        const BSONObj& command,
                        int options,
                        const string& versionedNS,
                        const BSONObj& targetingQuery,
                        vector<CommandResult>* results );

        /**
         * Executes a command represented in the Request on the sharded cluster.
         *
         * DEPRECATED: should not be used by new code.
         */
        void clientCommandOp( Request& r );

    protected:

        void doIndexQuery( Request& r , const Shard& shard );

        bool handleSpecialNamespaces( Request& r , QueryMessage& q );

    };

    extern Strategy* STRATEGY;

}

