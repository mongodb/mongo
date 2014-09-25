/**
 *    Copyright (C) 2014 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/db/commands.h"

namespace mongo {

    /**
     * Implements the explain command on mongos.
     *
     * "Old-style" explains (i.e. queries which have the $explain flag set), do not run
     * through this path. Such explains will be supported for backwards compatibility,
     * and must succeed in multiversion clusters.
     *
     * "New-style" explains use the explain command. When the explain command is routed
     * through mongos, it is forwarded to all relevant shards. If *any* shard does not
     * support a new-style explain, then the entire explain will fail (i.e. new-style
     * explains cannot be used in multiversion clusters).
     */
    class ClusterExplainCmd : public Command {
    MONGO_DISALLOW_COPYING(ClusterExplainCmd);
    public:
        ClusterExplainCmd() : Command("explain") { }

        virtual bool isWriteCommandForConfigServer() const { return false; }

        /**
         * Running an explain on a secondary requires explicitly setting slaveOk.
         */
        virtual bool slaveOk() const { return false; }
        virtual bool slaveOverrideOk() const { return true; }

        virtual bool maintenanceOk() const { return false; }

        virtual bool adminOnly() const { return false; }

        virtual void help( stringstream& help ) const {
            help << "explain database reads and writes";
        }

        /**
         * You are authorized to run an explain if you are authorized to run
         * the command that you are explaining. The auth check is performed recursively
         * on the nested command.
         */
        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj);

        virtual bool run(OperationContext* txn, const string& dbName,
                         BSONObj& cmdObj,
                         int options,
                         string& errmsg,
                         BSONObjBuilder& result,
                         bool fromRepl);

    };

} // namespace mongo
