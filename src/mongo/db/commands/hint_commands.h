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
#include "mongo/db/query/query_settings.h"
#include "mongo/db/query/plan_cache.h"

namespace mongo {

    /**
     * DB commands for admin hints.
     * Admin hint commands work on a different data structure in the collection
     * info cache from the plan cache.
     * The user still thinks of admin hint commands as part of the plan cache functionality
     * so the command name prefix is still "planCache".
     *
     * These are in a header to facilitate unit testing. See hint_commands_test.cpp.
     */

    /**
     * HintCommand
     * Defines common attributes for all admin hint related commands
     * such as slaveOk and locktype.
     */
    class HintCommand : public Command {
    public:
        HintCommand(const std::string& name, const std::string& helpText);

        /**
         * Entry point from command subsystem.
         * Implementation provides standardization of error handling
         * such as adding error code and message to BSON result.
         *
         * Do not override in derived classes.
         * Override runPlanCacheCommands instead to
         * implement plan cache command functionality.
         */

        bool run(const std::string& dbname, BSONObj& cmdObj, int options,
                 std::string& errmsg, BSONObjBuilder& result, bool fromRepl);

        /**
         * It's fine to return NONE here because plan cache commands
         * create explicit read context to access collection info cache.
         * Refer to dbcommands.cpp on how locktype() is handled.
         */
        virtual LockType locktype() const;

        virtual bool slaveOk() const;

        virtual void help(std::stringstream& ss) const;

        /**
         * One action type defined for hint commands:
         * - adminHintReadWrite
         */
        virtual Status checkAuthForCommand(ClientBasic* client, const std::string& dbname,
                                           const BSONObj& cmdObj);

        /**
         * Subset of command arguments used by hint commands
         * Override to provide command functionality.
         * Should contain just enough logic to invoke run*Command() function
         * in admin_hint.h
         */
        virtual Status runHintCommand(const std::string& ns, BSONObj& cmdObj,
                                      BSONObjBuilder* bob) = 0;

    private:
        std::string helpText;
    };

    /**
     * ListHints
     *
     * { planCacheListHints: <collection> }
     *
     */
    class ListHints : public HintCommand {
    public:
        ListHints();

        virtual Status runHintCommand(const std::string& ns, BSONObj& cmdObj, BSONObjBuilder* bob);

        /**
         * Looks up admin hints from collection's query settings.
         * Inserts hints into BSON builder.
         */
        static Status list(const QuerySettings& querySettings, BSONObjBuilder* bob);
    };

    /**
     * ClearHints
     *
     * { planCacheClearHints: <collection>, query: <query>, sort: <sort>, projection: <projection> }
     *
     */
    class ClearHints : public HintCommand {
    public:
        ClearHints();

        virtual Status runHintCommand(const std::string& ns, BSONObj& cmdObj, BSONObjBuilder* bob);

        /**
         * If query shape is provided, clears hints for a query.
         * Otherwise, clears collection's query settings.
         * Namespace argument ns is ignored if we are clearing the entire cache.
         * Removes corresponding entries from plan cache.
         */
        static Status clear(QuerySettings* querySettings, PlanCache* planCache, const std::string& ns,
                            const BSONObj& cmdObj);
    };

    /**
     * SetHint
     *
     * {
     *     planCacheSetHint: <collection>,
     *     query: <query>,
     *     sort: <sort>,
     *     projection: <projection>,
     *     indexes: [ <index1>, <index2>, <index3>, ... ]
     * }
     *
     */
    class SetHint : public HintCommand {
    public:
        SetHint();

        virtual Status runHintCommand(const std::string& ns, BSONObj& cmdObj, BSONObjBuilder* bob);

        /**
         * Sets admin hints for a query shape.
         * Removes entry for query shape from plan cache.
         */
        static Status set(QuerySettings* querySettings, PlanCache* planCache, const std::string& ns,
                          const BSONObj& cmdObj);
    };

}  // namespace mongo
