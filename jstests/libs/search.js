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
 *  results as the other mongots do not have an index (and all search queries require index). The
 *  solution is to run search index management commands directly on each shard rather than on the
 *  collection, to ensure the command is propogated to each mongot in the cluster. To do so, the
 *  javscript helper of each search index command calls DiscoverTopology.findConnectedNodes() and
 *  runs the index command on each shard individually.
 *
 *  It is important to note that the search index command isn't forwarded to the config server or
 *  to mongos. The former doesn't communicate with mongot. And as for the latter, resmoke's
 *  ShardedClusterFixture connects a sharded cluster's mongos to last launched mongot. In other
 *  words, mongos and one of the mongods share a mongot therefore issueing the search index command
 *  on the mongos would be redundant.
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

function _runUpdateSearchIndexOnShard(coll, keys, blockOnIndexQueryable, shardConn) {
    let shardDB = shardConn != undefined
        ? shardConn.getDB("admin").getSiblingDB(coll.getDB().getName())
        : coll.getDB();
    let collName = coll.getName();
    const name = keys["name"];
    let response = assert.commandWorked(
        shardDB.runCommand({updateSearchIndex: collName, name, definition: keys["definition"]}));

    if (!blockOnIndexQueryable) {
        return assert.commandWorked(response);
    }
    let statusUpdatedSearchIndex =
        shardDB[collName].aggregate([{$listSearchIndexes: {name}}]).toArray();
    // TODO SERVER-92200 renable the commented out assert and logic as mongot should have wiped
    // all non-existent index entries.
    // assert.eq(searchIndexArray.length, 1, searchIndexArray);
    // let queryable = searchIndexArray[0]["queryable"];

    // if (queryable) {
    //     return response;
    // }
    // assert.eq(statusUpdatedSearchIndex[0]["latestDefinition"], keys["definition"]);

    let searchIndexId;
    for (const {id, status, queryable} of statusUpdatedSearchIndex) {
        if (status != "DOES_NOT_EXIST") {
            if (queryable) {
                return response;
            }
            searchIndexId = id;
            break;
        }
    }

    // This default times out in 90 seconds.
    // TODO SERVER-92200 query by name and not ID.
    assert.soon(() => shardDB[collName]
                          .aggregate([{$listSearchIndexes: {id: searchIndexId}}])
                          .toArray()[0]["queryable"]);
    return assert.commandWorked(response);
}

export function updateSearchIndex(coll, keys, blockUntilSearchIndexQueryable = {
    "blockUntilSearchIndexQueryable": true
}) {
    _validateSearchIndexArguments("updateSearchIndex", keys, blockUntilSearchIndexQueryable);
    let blockOnIndexQueryable = blockUntilSearchIndexQueryable["blockUntilSearchIndexQueryable"];
    // Please see block comment at the top of this file to understand the sharded implementation.
    if (FixtureHelpers.isSharded(coll)) {
        let response = {};
        let topology = DiscoverTopology.findConnectedNodes(coll.getDB().getMongo());

        for (const shardName of Object.keys(topology.shards)) {
            topology.shards[shardName].nodes.forEach((node) => {
                let sconn = new Mongo(node);
                response = _runUpdateSearchIndexOnShard(coll, keys, blockOnIndexQueryable, sconn);
            });
        }
        return response;
    }
    return _runUpdateSearchIndexOnShard(coll, keys, blockOnIndexQueryable);
}
