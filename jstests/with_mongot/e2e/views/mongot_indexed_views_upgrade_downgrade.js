/**
 * This test verifies that search index commands and search queries on views behave correctly when
 * the FCV is upgraded and downgraded.
 *
 * TODO SERVER-92932: Delete this file once the feature flag is removed.
 *
 * @tags: [ requires_fcv_81 ]
 */
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";

const testDb = db.getSiblingDB(jsTestName());
const coll = testDb.underlyingSourceCollection;
coll.drop();

assert.commandWorked(coll.insertMany([{_id: 1, state: "NY", text: "hello new york"}]));

const addFieldsViewName = "addFieldsView";
assert.commandWorked(testDb.createView(addFieldsViewName, coll.getName(), [{$addFields: {country: "USA"}}]));
const addFieldsView = testDb[addFieldsViewName];

const identityViewName = "identityView";
assert.commandWorked(testDb.createView(identityViewName, coll.getName(), []));

const searchIndexDef = {
    name: "index",
    definition: {mappings: {dynamic: true}},
};

const vectorSearchIndexDef = {
    name: "index",
    type: "vectorSearch",
    definition: {fields: [{type: "vector", numDimensions: 10, path: "path", similarity: "euclidean"}]},
};

const adminDB = testDb.getMongo().getDB("admin");

/**
 * Drops all v4 2dsphere indexes from all collections in all databases.
 * This is necessary before downgrading FCV below 8.3, as v4 indexes are not supported.
 */
function dropAllV4_2dsphereIndexes() {
    const mongo = adminDB.getMongo();
    const dbNames = mongo.getDBNames();

    for (const dbName of dbNames) {
        const db = mongo.getDB(dbName);
        // Use getCollectionInfos to get only collections (not views)
        const collInfos = db.getCollectionInfos({type: "collection"});

        for (const collInfo of collInfos) {
            const collName = collInfo.name;
            // Skip system collections
            if (collName.startsWith("system.")) {
                continue;
            }

            const coll = db.getCollection(collName);
            const indexes = coll.getIndexes();

            for (const index of indexes) {
                // Check if this is a v4 2dsphere index
                if (index.key) {
                    const keyFields = Object.keys(index.key);
                    const has2dsphere = keyFields.some((field) => index.key[field] === "2dsphere");

                    if (has2dsphere && index["2dsphereIndexVersion"] === 4) {
                        jsTest.log.info(
                            "Dropping v4 2dsphere index: " + index.name + " on collection " + dbName + "." + collName,
                        );
                        assert.commandWorked(coll.dropIndex(index.name));
                    }
                }
            }
        }
    }
}

function upgradeDowngradeFCV(commands) {
    // Drop any v4 2dsphere indexes before downgrading, as they are not supported below FCV 8.3.
    dropAllV4_2dsphereIndexes();

    // Downgrade to lastLTSFCV (8.0).
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));

    // On 8.0 single node, search index commands on views should be blocked.
    if (!FixtureHelpers.isMongos(testDb)) {
        assert.commandFailedWithCode(
            testDb.runCommand({createSearchIndexes: addFieldsViewName, indexes: [searchIndexDef]}),
            ErrorCodes.QueryFeatureNotAllowed,
        );
        assert.commandFailedWithCode(
            testDb.runCommand({createSearchIndexes: addFieldsViewName, indexes: [vectorSearchIndexDef]}),
            ErrorCodes.QueryFeatureNotAllowed,
        );
    }

    // All commands should be blocked on 8.0.
    for (const command of commands) {
        assert.commandFailedWithCode(testDb.runCommand(command), ErrorCodes.OptionNotSupportedOnView);
    }

    // An identity view search aggregation *should work* on 8.0.
    assert.commandWorked(testDb.runCommand({aggregate: identityViewName, pipeline: searchPipeline, cursor: {}}));

    // Upgrade to latestFCV.
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));

    // All commands should work on latestFCV.
    for (const command of commands) {
        assert.commandWorked(testDb.runCommand(command));
    }

    // Search index commands should work on latestFCV.
    createSearchIndex(addFieldsView, searchIndexDef);
    dropSearchIndex(addFieldsView, {name: searchIndexDef.name});

    createSearchIndex(addFieldsView, vectorSearchIndexDef);
    dropSearchIndex(addFieldsView, {name: vectorSearchIndexDef.name});
}

const searchPipeline = [{$search: {index: "index", text: {query: "query", path: "path"}}}];
const vectorSearchPipeline = [
    {$vectorSearch: {index: "index", queryVector: [1, 2, 3], path: "path", limit: 1, exact: true}},
];

// Each of the commands passed through should work on latestFCV (FF enabled) but block on 8.0 (FF
// disabled).
upgradeDowngradeFCV([
    {aggregate: addFieldsViewName, pipeline: searchPipeline, cursor: {}},
    {aggregate: addFieldsViewName, pipeline: vectorSearchPipeline, cursor: {}},
    {
        // Basic $search $unionWith.
        aggregate: coll.getName(),
        pipeline: [{$unionWith: {coll: addFieldsViewName, pipeline: searchPipeline}}],
        cursor: {},
    },
    {
        // Basic $search $lookup.
        aggregate: coll.getName(),
        pipeline: [{$lookup: {from: addFieldsViewName, as: "lookup", pipeline: searchPipeline}}],
        cursor: {},
    },
    {
        // Basic $vectorSearch $unionWith.
        aggregate: coll.getName(),
        pipeline: [{$unionWith: {coll: addFieldsViewName, pipeline: vectorSearchPipeline}}],
        cursor: {},
    },
    {
        // Nested $search $lookup.
        aggregate: coll.getName(),
        pipeline: [
            {
                $lookup: {
                    from: coll.getName(),
                    as: "lookup",
                    pipeline: [{$lookup: {from: addFieldsViewName, as: "lookup", pipeline: searchPipeline}}],
                },
            },
        ],
        cursor: {},
    },
    {
        // Nested $search $unionWith.
        aggregate: coll.getName(),
        pipeline: [
            {
                $unionWith: {
                    coll: coll.getName(),
                    pipeline: [{$unionWith: {coll: addFieldsViewName, pipeline: searchPipeline}}],
                },
            },
        ],
        cursor: {},
    },
    {
        // Nested $vectorSearch $unionWith.
        aggregate: coll.getName(),
        pipeline: [
            {
                $unionWith: {
                    coll: coll.getName(),
                    pipeline: [{$unionWith: {coll: addFieldsViewName, pipeline: vectorSearchPipeline}}],
                },
            },
        ],
        cursor: {},
    },
    // TODO SERVER-106849: Uncomment when $vectorSearch is supported in $lookup.
    // {
    //     // Basic $vectorSearch $lookup.
    //     aggregate: coll.getName(),
    //     pipeline: [{$lookup: {from: addFieldsViewName, as: "lookup", pipeline: vectorSearchPipeline}}],
    //     cursor: {}
    // },
    // {
    //     // Nested $vectorSearch $lookup.
    //     aggregate: coll.getName(),
    //     pipeline: [{
    //         $lookup: {
    //             from: coll.getName(),
    //             as: "lookup",
    //             pipeline: [{$lookup: {from: addFieldsViewName, as: "lookup", pipeline: vectorSearchPipeline}}]
    //         }
    //     }],
    //     cursor: {}
    // },
]);
