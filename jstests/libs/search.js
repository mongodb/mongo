import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

/**
 *  All search queries ($search, $vectorSearch, PlanShardedSearch) require a search index.
 *  Regardless of what a collection contains, a search query will return no results if there is
 *  no search index. Furthermore, in sharded clusters, mongos handles search index management
 *  commands. However, since mongos is spun up connected to a single mongot, it only sends the
 *  command to its colocated mongot. This is problematic for sharding with mongot-localdev as
 *  each mongod is deployed with its own mongot and (for server testing purposes) the mongos is
 *  connected to the last spun up mongot. In other words, the rest of the mongots in the cluster
 *  do not receive these index management commands and thus search queries will return incomplete
 *  results as the other mongots do not have an index (and all search queries require index).
 *
 *  The solution is to forward the search index command to every mongod. More specifically:
 *  1. The javascript search index helper calls the search index command on the collection.
 *  2. mongos receives the search index command, resolves the view name if necessary, and forwards
 *  the command to it's assigned mongot-localdev (eg searchIndexManagementHostAndPort) which it
 *  shares with the last spun up mongod.
 *  3. mongot completes the request and mongos retrieves the response.
 *  4. mongos replicates the search index command on every mongod. It does so by asynchronously
 *  multicasting _shardsvrRunSearchIndexCommand (with the original user command, the
 *  alreadyInformedMongot hostAndPort, and the optional resolved view name) on every mongod in the
 *  cluster.
 *  5. Each mongod receives the _shardsvrRunSearchIndexCommand command. If this mongod shares its
 *  mongot with mongos, it does nothing as its mongot has already received the search index command.
 *  Otherwise, mongod calls runSearchIndexCommand with the necessary parameters forwarded from
 *  mongos.
 *  5. Once every mongod has forwarded the search index command, mongos returns the response from
 *  step 3.
 *
 *  It is important to note that the search index command isn't forwarded to the config server. The
 *  former doesn't communicate with mongot.
 */

/**
 * The create and update search index commands accept the same arguments. As such, they can share a
 * validation function. This function is called by search index command javascript helpers, it is
 * not intended to be called directly in a jstest.
 * @param {String} searchIndexCommandName Exclusively used for error messages, provided by the
 *     javascript implementation of the search index command.
 * @param {Object} keys Name(s) and definitions of the desired search indexes.
 * @param {Object} blockUntilSearchIndexQueryable Object that represents how the
 */
function _validateSearchIndexArguments(
    searchIndexCommandName, keys, blockUntilSearchIndexQueryable) {
    if (!keys.hasOwnProperty('definition')) {
        throw new Error(searchIndexCommandName + " must have a definition");
    }

    if (typeof (blockUntilSearchIndexQueryable) != 'object' ||
        Object.keys(blockUntilSearchIndexQueryable).length != 1 ||
        !blockUntilSearchIndexQueryable.hasOwnProperty('blockUntilSearchIndexQueryable')) {
        throw new Error(
            searchIndexCommandName +
            " only accepts index definition object and blockUntilSearchIndexQueryable object");
    }

    if (typeof (blockUntilSearchIndexQueryable["blockUntilSearchIndexQueryable"]) != "boolean") {
        throw new Error("'blockUntilSearchIndexQueryable' argument must be a boolean");
    }
}

function _runListSearchIndexOnMongod(
    coll, keys, blockOnIndexQueryable, mongodConn, latestDefinition) {
    let name = keys["name"];
    let dbName = coll.getDB().getName();
    let collName = coll.getName();

    let testColl = mongodConn != undefined ? mongodConn.getDB(dbName)[collName] : coll;

    let searchIndexArray = testColl.aggregate([{$listSearchIndexes: {name}}]).toArray();

    assert.eq(searchIndexArray.length, 1, searchIndexArray);

    if (latestDefinition != null) {
        /**
         * We're running $listSearchIndexes after an update, need to confirm that we're looking at
         * index entry for latest definition.
         */
        assert.eq(searchIndexArray[0].latestDefinition, latestDefinition);
    }

    let queryable = searchIndexArray[0]["queryable"];

    if (queryable) {
        return;
    }

    assert.soon(() => testColl.aggregate([{$listSearchIndexes: {name}}]).toArray()[0]["queryable"]);
}

export function updateSearchIndex(coll, keys, blockUntilSearchIndexQueryable = {
    "blockUntilSearchIndexQueryable": true
}) {
    _validateSearchIndexArguments("updateSearchIndex", keys, blockUntilSearchIndexQueryable);
    let blockOnIndexQueryable = blockUntilSearchIndexQueryable["blockUntilSearchIndexQueryable"];

    const name = keys["name"];
    let response = assert.commandWorked(
        coll.runCommand({updateSearchIndex: coll.getName(), name, definition: keys["definition"]}));
    let topology = DiscoverTopology.findConnectedNodes(coll.getDB().getMongo());
    // Please see block comment at the top of this file to understand the sharded implementation.
    if (FixtureHelpers.isSharded(coll)) {
        let response = {};
        // Call $listSearchIndex on every mongod to ensure the create command was propogated to each
        // mongod (and therefore to each mongot).
        for (const shardName of Object.keys(topology.shards)) {
            topology.shards[shardName].nodes.forEach((node) => {
                let sconn = new Mongo(node);
                // To ensure we return the initial response from calling the specified search index
                // command (in this case create), we do not modify response here with these
                // listSearchIndex calls on a specified host.
                _runListSearchIndexOnMongod(
                    coll, keys, blockOnIndexQueryable, sconn, keys["definition"]);
            });
        }
    } else {
        // To ensure we return the initial response from calling the specified search index command
        // (in this case create), we do not modify response here with this listSearchIndex call.
        _runListSearchIndexOnMongod(coll, keys, blockOnIndexQueryable, null, keys["definition"]);
    }
    return response;
}
function _runDropSearchIndexOnShard(coll, keys, shardConn) {
    let shardDB = shardConn != undefined
        ? shardConn.getDB("admin").getSiblingDB(coll.getDB().getName())
        : coll.getDB();
    let collName = coll.getName();

    let name = keys["name"];
    return assert.commandWorked(shardDB.runCommand({dropSearchIndex: collName, name}));
}

export function dropSearchIndex(coll, keys) {
    if (Object.keys(keys).length != 1 || !keys.hasOwnProperty('name')) {
        /**
         * dropSearchIndex server command accepts search index ID or name. However, the
         * createSearchIndex library helper only returns the response from issuing the creation
         * command on the last shard. This is problematic for sharded configurations as a server dev
         * won't have all the IDs associated with the search index across all of the shards. To
         * ensure correctness, the dropSearchIndex library helper will only accept specifiying
         * search index by name.
         */
        throw new Error("dropSearchIndex library helper only accepts a search index name");
    }
    let name = keys["name"];
    return assert.commandWorked(coll.getDB().runCommand({dropSearchIndex: coll.getName(), name}));
}

export function createSearchIndex(coll, keys, blockUntilSearchIndexQueryable) {
    if (arguments.length > 3) {
        throw new Error("createSearchIndex accepts up to 3 arguments");
    }

    let blockOnIndexQueryable = true;
    if (arguments.length == 3) {
        // The third arg may only be the "blockUntilSearchIndexQueryable" flag.
        if (typeof (blockUntilSearchIndexQueryable) != 'object' ||
            Object.keys(blockUntilSearchIndexQueryable).length != 1 ||
            !blockUntilSearchIndexQueryable.hasOwnProperty('blockUntilSearchIndexQueryable')) {
            throw new Error(
                "createSearchIndex only accepts index definition object and blockUntilSearchIndexQueryable object");
        }

        blockOnIndexQueryable = blockUntilSearchIndexQueryable["blockUntilSearchIndexQueryable"];
        if (typeof blockOnIndexQueryable != "boolean") {
            throw new Error("'blockUntilSearchIndexQueryable' argument must be a boolean");
        }
    }

    if (!keys.hasOwnProperty('definition')) {
        throw new Error("createSearchIndex must have a definition");
    }

    let response = assert.commandWorked(
        coll.getDB().runCommand({createSearchIndexes: coll.getName(), indexes: [keys]}));
    let topology = DiscoverTopology.findConnectedNodes(coll.getDB().getMongo());
    // Please see block comment at the top of this file to understand the sharded implementation.
    if (FixtureHelpers.isSharded(coll)) {
        // Call $listSearchIndex on every mongod to ensure the create command was propogated to each
        // mongod (and therefore to each mongot).
        for (const shardName of Object.keys(topology.shards)) {
            topology.shards[shardName].nodes.forEach((node) => {
                let sconn = new Mongo(node);
                // To ensure we return the initial response from calling the specified search index
                // command (in this case create), we do not modify response here with these
                // listSearchIndex calls on a specified host.
                _runListSearchIndexOnMongod(coll, keys, blockOnIndexQueryable, sconn);
            });
        }
    } else {
        // To ensure we return the initial response from calling the specified search index command
        // (in this case create), we do not modify response here with this listSearchIndex call.
        _runListSearchIndexOnMongod(coll, keys, blockOnIndexQueryable);
    }

    return response;
}