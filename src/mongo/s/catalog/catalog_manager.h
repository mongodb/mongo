/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include <boost/shared_ptr.hpp>
#include <string>

#include "mongo/base/disallow_copying.h"

namespace mongo {

    class BatchedCommandRequest;
    class BatchedCommandResponse;
    class BSONObj;
    class DatabaseType;
    class OperationContext;
    class Status;
    template<typename T> class StatusWith;

    
    /**
     * Abstracts reads and writes of the sharding catalog metadata.
     *
     * All implementations of this interface should go directly to the persistent backing store
     * and should avoid doing any caching of their own. The caching is delegated to a parallel
     * read-only view of the catalog, which is maintained by a higher level code.
     */
    class CatalogManager {
        MONGO_DISALLOW_COPYING(CatalogManager);
    public:
        virtual ~CatalogManager() = default;

        /**
         * Creates a new database or updates the sharding status for an existing one. Cannot be
         * used for the admin/config/local DBs, which should not be created or sharded manually
         * anyways.
         *
         * Returns Status::OK on success or any error code indicating the failure. These are some
         * of the known failures:
         *  - DatabaseDifferCaseCode - database already exists, but with a different case
         *  - ShardNotFound - could not find a shard to place the DB on
         */
        virtual Status enableSharding(const std::string& dbName) = 0;

        /**
         * Updates the metadata for a given database. Currently, if the specified DB entry does
         * not exist, it will be created.
         */
        virtual Status updateDatabase(const std::string& dbName, const DatabaseType& db) = 0;

        /**
         * Retrieves the metadata for a given database.
         */
        virtual StatusWith<DatabaseType> getDatabase(const std::string& dbName) = 0;

        /**
         * Logs a diagnostic event locally and on the config server.
         *
         * NOTE: This method is best effort so it should never throw.
         *
         * @param opCtx The operation context of the call doing the logging
         * @param what E.g. "split", "migrate"
         * @param ns To which collection the metadata change is being applied
         * @param detail Additional info about the metadata change (not interpreted)
         */
        virtual void logChange(OperationContext* opCtx,
                               const std::string& what,
                               const std::string& ns,
                               const BSONObj& detail) = 0;

        /**
         * Directly sends the specified command to the config server and returns the response.
         *
         * NOTE: Usage of this function is disallowed in new code, which should instead go through
         *       the regular catalog management calls. It is currently only used privately by this
         *       class and externally for writes to the admin/config namespaces.
         *
         * @param request Request to be sent to the config server.
         * @param response Out parameter to receive the response. Can be NULL.
         */
        virtual void writeConfigServerDirect(const BatchedCommandRequest& request,
                                             BatchedCommandResponse* response) = 0;


        /**
         * Directly inserts a document in the specified namespace on the config server (only the
         * config or admin databases). If the document does not have _id field, the field will be
         * added.
         *
         * NOTE: Should not be used in new code. Instead add a new metadata operation to the
         *       interface.
         */
        Status insert(const std::string& ns,
                      const BSONObj& doc,
                      BatchedCommandResponse* response);

        /**
         * Updates a document in the specified namespace on the config server (only the config or
         * admin databases).
         */
        Status update(const std::string& ns,
                      const BSONObj& query,
                      const BSONObj& update,
                      bool upsert,
                      bool multi,
                      BatchedCommandResponse* response);

        /**
         * Removes a document from the specified namespace on the config server (only the config
         * or admin databases).
         *
         * NOTE: Should not be used in new code. Instead add a new metadata operation to the
         *       interface.
         */
        Status remove(const std::string& ns,
                      const BSONObj& query,
                      int limit,
                      BatchedCommandResponse* response);

    protected:
        CatalogManager() = default;
    };

} // namespace mongo
