import {getCollectionName} from "jstests/libs/cmd_object_utils.js";
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

/**
 *  All search queries ($search, $vectorSearch, PlanShardedSearch) require a search index.
 *  Regardless of what a collection contains, a search query will return no results if there is
 *  no search index. Furthermore, in sharded clusters, the router handles search index management
 *  commands exclusively. However, since the router is spun up connected to a single mongot, it only
 * sends the command to its colocated mongot. This is problematic for sharding with mongot-localdev
 * as each mongod is deployed with its own mongot and (for server testing purposes) the router is
 *  connected to the last spun up mongot. In other words, the rest of the mongots in the cluster
 *  do not receive these index management commands and thus search queries will return incomplete
 *  results as the other mongots do not have an index (and all search queries require index).
 *
 *  The solution is to forward the search index command to every mongod. More specifically:
 *  1. The javascript search index command helper calls the search index command on the request nss.
 *  2. The router receives the search index command, resolves the view name if necessary, and
 * forwards the command to its assigned mongot-localdev (e.g. searchIndexManagementHostAndPort)
 * which it shares with the last spun up mongod.
 *  3. mongot completes the request and the router retrieves and returns the response.
 *  4. The javascript search index helper calls _runAndReplicateSearchIndexCommand(), which sends a
 *  replicateSearchIndexCommand to the router with the original user command.
 *  5. replicateSearchIndexCommand::typedRun() calls
 *  search_index_testing_helper::_replicateSearchIndexCommandOnAllMongodsForTesting(). This helper
 *  asynchronously multicasts _shardsvrRunSearchIndexCommand (which includes the original user
 *  command, the alreadyInformedMongot hostAndPort, and the optional resolved view name) on every
 *  mongod in the cluster.
 *  6. Each mongod receives the _shardsvrRunSearchIndexCommand command. If this mongod shares its
 *  mongot with the router, it does nothing as its mongot has already received the search index
 * command. Otherwise, mongod calls runSearchIndexCommand with the necessary parameters forwarded
 * from the router.
 *  7. After every mongod has been issued the _shardsvrRunSearchIndexCommand,
 *  search_index_testing_helper::_replicateSearchIndexCommandOnAllMongodsForTesting() then issues a
 *  $listSearchIndex command on every mongod until every mongod reports that the specified index is
 *  queryable. It will return once the index is queryable across the entire cluster and throw an
 *  error otherwise.
 *  8. The javascript search index command helper returns the response from step 3.
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

function isShardedView(coll) {
    // The isSharded function identifies a sharded collection by looking for an entry in the config
    // database for the given nss. However, the view nss is not entered in the config database. For
    // this reason, we have to resolve the view and then check the config database on the resolved
    // nss to identify if the query is being run on a sharded view.
    let db = coll.getDB();
    if (FixtureHelpers.getViewDefinition(db, coll.getName())) {
        let sourceColl = getCollectionName(db, coll.getName());
        if (FixtureHelpers.isSharded(db[sourceColl])) {
            return true;
        }
    }
    return false;
}

function isShardedHelper(coll) {
    if (FixtureHelpers.isSharded(coll) || isShardedView(coll)) {
        return true;
    }
    return false;
}

function _runListSearchIndexOnNode(coll, indexName, latestDefinition) {
    let name = indexName;
    let dbName = coll.getDB().getName();
    let collName = coll.getName();
    let searchIndexArray = coll.aggregate([{$listSearchIndexes: {name}}]).toArray();
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

    assert.soon(() => {
        searchIndexArray = coll.aggregate([{$listSearchIndexes: {name}}]).toArray();
        if (searchIndexArray[0]["queryable"]) {
            if (latestDefinition == null) {
                return true;
            }
            return bsonWoCompare(searchIndexArray[0].latestDefinition, latestDefinition) === 0;
        }
    });
}

function _runAndReplicateSearchIndexCommand(coll, userCmd, indexName, latestDefinition = null) {
    let response = assert.commandWorked(coll.getDB().runCommand(userCmd));
    // Please see block comment at the top of this file to understand the sharded implementation.
    if (isShardedHelper(coll)) {
        assert.commandWorked(
            coll.getDB().runCommand({replicateSearchIndexCommand: coll.getName(), userCmd}));
    } else {
        // dropSearchIndex returns once the index is deleted so no need to run $listSearchIndexes.
        if (Object.keys(userCmd)[0] != 'dropSearchIndex') {
            _runListSearchIndexOnNode(coll, indexName, latestDefinition);
        }
    }
    return response;
}

export function updateSearchIndex(coll, keys, blockUntilSearchIndexQueryable = {
    "blockUntilSearchIndexQueryable": true
}) {
    _validateSearchIndexArguments("updateSearchIndex", keys, blockUntilSearchIndexQueryable);
    let blockOnIndexQueryable = blockUntilSearchIndexQueryable["blockUntilSearchIndexQueryable"];

    const name = keys["name"];
    let userCmd = {updateSearchIndex: coll.getName(), name, definition: keys["definition"]};
    return _runAndReplicateSearchIndexCommand(coll, userCmd, name, keys["definition"]);
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
    let userCmd = {dropSearchIndex: coll.getName(), name};
    return _runAndReplicateSearchIndexCommand(coll, userCmd, name);
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

    let userCmd = {createSearchIndexes: coll.getName(), indexes: [keys]};
    let name = "default";
    if ("name" in keys) {
        name = keys["name"];
    }

    return _runAndReplicateSearchIndexCommand(coll, userCmd, name);
}