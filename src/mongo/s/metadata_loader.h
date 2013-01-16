/**
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

#include <string>

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/jsobj.h"

namespace mongo {

    class CollectionManager;
    class CollectionType;
    class DBClientCursor;

    /**
     * The MetadataLoader is responsible for interfacing with the config servers and obtaining
     * the data that CollectionManagers are made of. Effectively, the loader is the "builder"
     * class for that manager.
     *
     * The loader will go out of its way to try to fetch the smaller amount possible of data
     * from the config server without sacrificing the freshness and accuracy of the managers is
     * builds. (See ConfigDiffTracker class.)
     *
     * The class is not thread safe.
     */
    class MetadataLoader {
    public:

        /**
         * Takes a connection string to the config servers to be used for loading data. Note
         * that we make no restrictions about which connection string that is, including
         * CUSTOM, which we rely on in testing.
         */
        explicit MetadataLoader(ConnectionString configLoc);

        ~MetadataLoader();

        /**
         * Returns a new manager's instance representing the chunkset of the collection 'ns'
         * (or its entirety, if not sharded) that lives on 'shard'. Optionally, uses an
         * 'oldManager' for the same 'ns'/'shard'; the contents of 'oldManger' can help
         * reducing the amount of data read from the config servers.
         *
         * If the collection's information can't be loaded, returns NULL and fill in 'errMsg'
         * with a description, if 'errMsg' was provided.
         */
        CollectionManager* makeCollectionManager(const string& ns,
                                                 const string& shard,
                                                 const CollectionManager* oldManager,
                                                 string* errMsg);

        /**
         * Returns a new manager's instance representing an non-sharded, empty collection with
         * the initial version number (1|0|oid).
         */
        CollectionManager* makeEmptyCollectionManager();

    private:
        ConnectionString _configLoc;

        /**
         * Returns true and fills in the internal state of 'manager' to portray the portion of
         * the collection 'ns' that lives in 'shard'. If provided, uses the contents of
         * 'oldManager' as a base, which allows less data to be brought from the config
         * server. If information about the collection can be accessed or is invalid, returns
         * false and fills in an error description on '*errMsg', which is mandatory here.
         */
        bool initCollection(const string& ns,
                            const string& shard,
                            const CollectionManager* oldManager,
                            CollectionManager* manager,
                            string* errMsg);

        /**
         * Returns true and fills in the chunk state of 'manager' to portray the chunks of the
         * collection 'ns' that sit in 'shard'. If provided, uses the contents of 'oldManager'
         * as a base (see description in initCollection above). If information about the
         * chunks can be accessed or is invalid, returns false and fills in an error
         * description on '*errMsg', which is mandatory here.
         */
        bool initChunks(const CollectionType& collDoc,
                        const string& ns,
                        const string& shard,
                        const CollectionManager* oldManager,
                        CollectionManager* manager,
                        string* errMsg);
    };

} // namespace mongo
