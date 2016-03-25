/**
 * Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"

namespace mongo {

class OperationContext;
class PlanExecutor;

/**
 * LocalClient handles local -- not over the network -- queries and writes. It acquires all
 * necessary collection locks: these locks should not be held by the caller.
 *
 * LocalClient is associated with a particular OperationContext and should not be passed between
 * threads.
 */
class LocalClient {
    MONGO_DISALLOW_COPYING(LocalClient);

public:
    class LocalCursor;

    LocalClient(OperationContext* txn);

    /**
     * Sets up a local find query. Returns a LocalCursor object from which to request the next
     * document that matches "query" in order "sort" from collection "nss".
     *
     * The caller should not hold the collection lock: locking is handled internally.
     */
    StatusWith<LocalCursor> query(const NamespaceString& nss,
                                  const BSONObj& query,
                                  const BSONObj& sort);

    /**
     * LocalCursor sets up and executes a local find query.
     *
     * Collection locking is handled internally. Lock is released between next() calls.
     *
     * The caller need only request the next document after instantiating the LocalCursor and
     * calling init().
     */
    class LocalCursor {
    public:
        friend class LocalClient;

#if defined(_MSC_VER) && _MSC_VER < 1900
        LocalCursor(LocalCursor&& other);
        LocalCursor& operator=(LocalCursor&& other);
#else
        LocalCursor(LocalCursor&& other) = default;
        LocalCursor& operator=(LocalCursor&& other) = default;
#endif

        /**
         * Seeks the next document that matches the query. Returns an error Status or an optional
         * BSONObj. If optional is empty, there are no more documents that match "_query";
         * otherwise the next document is returned.
         *
         * ErrorCodes::OperationFailed is returned if the PlanExecutor encounters an unrecoverable
         * error or something happens to the collection -- such as an index or the collection
         * itself being dropped -- between calls or when the PlanExecutor yields.
         *
         * Locking is handled internally: expects the collection lock not to be held.
         */
        StatusWith<boost::optional<BSONObj>> next();

    private:
        /**
         * Instantiates a LocalCursor that returns documents from collection "nss" matching "query"
         * in the "sort" order.
         */
        LocalCursor(OperationContext* txn,
                    const NamespaceString& nss,
                    const BSONObj& query,
                    const BSONObj& sort);

        /**
         * Attempts to set up the find query, returns a Status to indicate success or failure.
         *
         * Locking is handled internally: expects the collection lock not to be held.
         */
        Status _init();

        OperationContext* _txn;

        // The collection in which to search.
        NamespaceString _nss;

        // Contains the query that selects documents to return.
        BSONObj _query;

        // Contains the sort order information.
        BSONObj _sort;

        // Find query PlanExecutor from which to request the next document.
        std::unique_ptr<PlanExecutor> _exec;
    };

private:
    OperationContext* const _txn;
};

}  // namespace mongo
