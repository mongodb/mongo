/**
 * Defines helpers for running commands on repeat. Used to test that query sampling respects the
 * sample rate configured via the 'configureQueryAnalyzer' command.
 */

load("jstests/sharding/analyze_shard_key/libs/query_sampling_util.js");

const fieldName = "x";

/**
 * Tries to run randomly generated find commands against the collection 'collName' in the database
 * 'dbName' at rate 'targetNumPerSec' for 'durationSecs'. Returns the actual rate.
 */
function runFindCmdsOnRepeat(host, dbName, collName, targetNumPerSec, durationSecs) {
    load("jstests/sharding/analyze_shard_key/libs/sample_rates_common.js");
    const conn = new Mongo(host);
    const db = conn.getDB(dbName);
    const makeCmdObjFunc = () => {
        const value = AnalyzeShardKeyUtil.getRandInteger(1, 500);
        // Use a range filter half of the time to get test coverage for targeting more than one
        // shard in the sharded case.
        const filter = Math.random() > 0.5 ? {[fieldName]: {$gte: value}} : {[fieldName]: value};
        const collation = QuerySamplingUtil.generateRandomCollation();
        return {find: collName, filter, collation, $readPreference: {mode: "secondaryPreferred"}};
    };
    return QuerySamplingUtil.runCmdsOnRepeat(db, makeCmdObjFunc, targetNumPerSec, durationSecs);
}

/**
 * Tries to run randomly generated delete commands against the collection 'collName' in the database
 * 'dbName' at rate 'targetNumPerSec' for 'durationSecs'. Returns the actual rate.
 */
function runDeleteCmdsOnRepeat(host, dbName, collName, targetNumPerSec, durationSecs) {
    load("jstests/sharding/analyze_shard_key/libs/sample_rates_common.js");
    const conn = new Mongo(host);
    const db = conn.getDB(dbName);
    const makeCmdObjFunc = () => {
        const value = AnalyzeShardKeyUtil.getRandInteger(1, 500);
        // Use a range filter half of the time to get test coverage for targeting more than one
        // shard in the sharded case.
        const filter = Math.random() > 0.5 ? {[fieldName]: {$gte: value}} : {[fieldName]: value};
        const collation = QuerySamplingUtil.generateRandomCollation();
        return {delete: collName, deletes: [{q: filter, collation, limit: 0}]};
    };
    return QuerySamplingUtil.runCmdsOnRepeat(db, makeCmdObjFunc, targetNumPerSec, durationSecs);
}

/**
 * Tries to run randomly generated aggregate commands with a $lookup stage against the collections
 * 'localCollName' and 'foreignCollName' in the database 'dbName' at rate 'targetNumPerSec' for
 * 'durationSecs'. Returns the actual rate.
 */
function runNestedAggregateCmdsOnRepeat(
    host, dbName, localCollName, foreignCollName, targetNumPerSec, durationSecs) {
    load("jstests/sharding/analyze_shard_key/libs/sample_rates_common.js");
    const conn = new Mongo(host);
    const db = conn.getDB(dbName);
    const makeCmdObjFunc = () => {
        const value = AnalyzeShardKeyUtil.getRandInteger(1, 500);
        // Use a range filter half of the time to get test coverage for targeting more than one
        // shard in the sharded case.
        const filter = Math.random() > 0.5 ? {[fieldName]: {$gte: value}} : {[fieldName]: value};
        const collation = QuerySamplingUtil.generateRandomCollation();
        return {
            aggregate: localCollName,
            pipeline:
                [{$lookup: {from: foreignCollName, as: "joined", pipeline: [{$match: filter}]}}],
            collation,
            cursor: {},
            $readPreference: {mode: "secondaryPreferred"}
        };
    };
    return QuerySamplingUtil.runCmdsOnRepeat(db, makeCmdObjFunc, targetNumPerSec, durationSecs);
}
