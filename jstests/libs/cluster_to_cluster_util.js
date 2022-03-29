/**
 * Utilities for testing cluster to cluster replicator.
 */
let ClusterToClusterUtil = (function() {
    load("jstests/libs/namespace_utils.js");

    // System databases and collections that are excluded from copying. We also exclude the
    // "mongosync" database since that stores metadata used by the replicator.
    const excludedSystemDatabases =
        ["admin", "config", "local", "mongosync_reserved_for_internal_use"];
    const excludedSystemCollections =
        ["system.views", "system.profile", "system.resharding.", "system.buckets.", "system.drop."];

    /**
     * Perform sanity check on the namespaces to filter.
     */
    function checkFilteredNamespacesInput(namespaces) {
        if (namespaces) {
            for (const ns of namespaces) {
                const [db, coll] = getDBNameAndCollNameFromFullNamespace(ns);
                assert(db && coll, `Incorrect namespace format: ${ns}`);
                assert(!excludedSystemDatabases.includes(db),
                       "Filtered namespaces cannot contain excluded system databases");
                assert(!coll.startsWith("system."));
            }
        }
    }

    /**
     * Return the databases to copy from the source cluster.
     */
    function getDatabasesToCopy(conn) {
        const listDBRes = assert.commandWorked(conn.adminCommand(
            {listDatabases: 1, filter: {name: {$nin: excludedSystemDatabases}}, nameOnly: true}));
        return listDBRes.databases.map(entry => entry.name);
    }

    /**
     * Return all the collections to copy from the source cluster, grouped by database and
     * filtered by includeNamespaces and excludeNamespaces. When includeNamespaces is not
     * provided, the collection infos returned will include views, so callers may need to
     * explicitly check the collection type to different between collections and views.
     */
    function getCollectionsToCopy(conn, includeNamespaces, excludeNamespaces) {
        const collInfoMap = {};

        if (includeNamespaces && includeNamespaces.length > 0) {
            assert(!excludeNamespaces || excludeNamespaces.size == 0,
                   "Cannot have inputs for both includeNamespaces and excludeNamespaces");
            checkFilteredNamespacesInput(includeNamespaces);

            for (const ns of includeNamespaces) {
                const [dbName, collName] = getDBNameAndCollNameFromFullNamespace(ns);
                const collInfo = getCollectionInfo(conn, dbName, collName);
                if (!collInfo) {
                    print(`Namespace to include for copy does not exist: ${dbName}.${collName}`);
                    continue;
                }
                if (!collInfoMap.hasOwnProperty(dbName)) {
                    collInfoMap[dbName] = [];
                }
                collInfoMap[dbName].push(collInfo);
            }

            return collInfoMap;
        }

        checkFilteredNamespacesInput(excludeNamespaces);
        const databases = getDatabasesToCopy(conn);
        databases.forEach(dbName => {
            const collInfos = getCollectionsFromDatabase(conn, dbName, excludeNamespaces);
            if (collInfos.length > 0) {
                collInfoMap[dbName] = collInfos;
            }
        });

        return collInfoMap;
    }

    /**
     * Return the collection infos of the given database, excluding those in the excludeNamespaces.
     */
    function getCollectionsFromDatabase(conn, dbName, excludeNamespaces = []) {
        let excludedCollections = excludeNamespaces.reduce((list, ns) => {
            const [db, coll] = getDBNameAndCollNameFromFullNamespace(ns);
            if (db === dbName) {
                list.push(coll);
            }
            return list;
        }, [...excludedSystemCollections]);

        excludedCollections = excludedCollections.map(coll => {
            // If collection ends with '.', match the prefix
            return coll.endsWith('.') ? new RegExp(`^${coll}`) : coll;
        });

        const res = assert.commandWorked(conn.getDB(dbName).runCommand(
            {listCollections: 1, filter: {name: {$nin: excludedCollections}}}));
        return new DBCommandCursor(db, res).toArray().sort(compareOn("name"));
    }

    /**
     * Return the collection info of the given collection name, or null if no such collection.
     */
    function getCollectionInfo(conn, dbName, collName) {
        const res = assert.commandWorked(
            conn.getDB(dbName).runCommand({listCollections: 1, filter: {name: collName}}));
        const firstBatch = res.cursor.firstBatch;
        return firstBatch.length > 0 ? firstBatch[0] : null;
    }

    /**
     * Return the shard key information of the given collection, or null if the collection
     * is not sharded.
     */
    function getShardKeyInfo(conn, dbName, collName) {
        return conn.getDB("config").collections.findOne({_id: `${dbName}.${collName}`});
    }

    return {
        checkFilteredNamespacesInput,
        getDatabasesToCopy,
        getCollectionsToCopy,
        getCollectionsFromDatabase,
        getCollectionInfo,
        getShardKeyInfo,
    };
})();
