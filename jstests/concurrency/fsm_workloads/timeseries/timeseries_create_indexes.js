/**
 * Repeatedly create indexes while dropping and recreating timeseries collections.
 *
 * @tags: [
 *   requires_timeseries,
 *   # TODO SERVER-105327 remove the following exclusion tag
 *   excluded_from_simulate_crash_suites,
 *   # TODO SERVER-105270 enable test in viewless timeseries suites
 *   does_not_support_viewless_timeseries_yet,
 *   # TODO SERVER-105492 enable test in multiversion suites
 *   multiversion_incompatible,
 *   # TODO SERVER-105509 enable test in config shard suites
 *   config_shard_incompatible,
 * ]
 */

import {
    uniformDistTransitions
} from "jstests/concurrency/fsm_workload_helpers/state_transition_utils.js";
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";

const dbPrefix = jsTestName() + '_DB_';
const dbCount = 2;
const collPrefix = 'coll_';
const collCount = 2;
const timeFieldName = "time";
const metaFieldName = "meta";

function getRandomDb(db) {
    return db.getSiblingDB(dbPrefix + Random.randInt(dbCount));
}

function getRandomCollection(db) {
    return db[collPrefix + Random.randInt(collCount)];
}

export const $config = (function() {
    var data = {nsPrefix: "create_idx_", numCollections: 5};

    var states = {
        createNormalColl: function(db, collname) {
            const coll = getRandomCollection(db);
            assert.commandWorkedOrFailedWithCode(db.createCollection(coll.getName()),
                                                 [ErrorCodes.NamespaceExists]);
        },
        createTimeseriesColl: function(db, collname) {
            const coll = getRandomCollection(db);
            assert.commandWorkedOrFailedWithCode(db.createCollection(coll.getName(), {
                timeseries: {timeField: timeFieldName, metaField: metaFieldName}
            }),
                                                 [ErrorCodes.NamespaceExists]);
        },
        insert: function(db, collName) {
            const coll = getRandomCollection(db);
            assert.commandWorkedOrFailedWithCode(
                coll.insert({
                    measurement: "measurement",
                    time: ISODate(),
                }),
                [
                    // The collection has been dropped and re-created as non-timeseries
                    // during the insert.
                    // TODO (SERVER-85548): revisit 85557* error codes
                    8555700,
                    8555701,
                    // The collection has been dropped and re-created as a time-series collection
                    // during our insert.
                    10551700,
                    // Collection UUID changed during insert 974880*
                    9748800,
                    9748801,
                    9748802,
                    // If the collection already exists and is a view
                    // TODO SERVER-85548 this should never happen
                    ErrorCodes.NamespaceExists,
                    // TODO SERVER-85548 this should never happen
                    ErrorCodes.CommandNotSupportedOnView,
                    // The buckets collection gets dropped while translating namespace
                    // from timeseries view.
                    // TODO SERVER-85548 remove after legacy timeseries
                    ErrorCodes.NamespaceNotFound,
                    ErrorCodes.NoProgressMade,
                ]);
        },
        drop: function(db, collName) {
            const coll = getRandomCollection(db);
            coll.drop();
        },
        createIndex: function(db, collName) {
            const allIndexSpecs = [
                {[metaFieldName]: 1},
                {[timeFieldName]: 1},
                {'measurement': 1},
                {'other': 1},
            ];

            const coll = getRandomCollection(db);
            const indexSpec = allIndexSpecs[Math.floor(Math.random() * allIndexSpecs.length)];

            jsTest.log(`Creating index '${tojsononeline(indexSpec)}' on ${coll.getFullName()}`);
            assert.commandWorkedOrFailedWithCode(coll.createIndex(indexSpec), [
                // The collection has been concurrently dropped and recreated
                ErrorCodes.CollectionUUIDMismatch,
                ErrorCodes.IndexBuildAborted,
                // If the collection already exists and is a view
                // TODO SERVER-85548 this should never happen
                ErrorCodes.NamespaceExists,
                // TODO SERVER-85548 this should never happen
                ErrorCodes.CommandNotSupportedOnView,
                // The buckets collection gets dropped while translating namespace
                // from timeseries view.
                // TODO SERVER-85548 remove after legacy timeseries
                ErrorCodes.NamespaceNotFound,
                // The buckets collection gets dropped and re-created as non-timeseries
                // during index build.
                5993303,
                // Encountered when two threads execute concurrently and repeatedly create
                // collection (as part of createIndexes) and drop collection
                ErrorCodes.CannotImplicitlyCreateCollection,
                // TODO SERVER-104712 remove the following exected error
                10195200,
            ]);
        },
        checkIndexes: function(db, collName) {
            const coll = getRandomCollection(db);
            const indexes = coll.getIndexes();
            indexes.forEach(index => {
                Object.keys(index.key).forEach(indexKey => {
                    assert(!indexKey.startsWith('control.'),
                           `Found buckets index spec on timeseries collection ${coll.getName()}: ${
                               tojson(index)}`);
                });
            });
        },
    };

    return {
        threadCount: 12,
        iterations: 64,
        states: states,
        startState: 'createTimeseriesColl',
        transitions: uniformDistTransitions(states),
    };
})();
