/**
 * This test confirms that query stats store key fields for an aggregate command are properly nested
 * and none are missing. It also validates the exact pipeline in the query shape.
 *  @tags: [
 *   uses_change_streams,
 *   requires_fcv_60
 * ]
 */

load("jstests/libs/query_stats_utils.js");         // For runCommandAndValidateQueryStats.
load("jstests/libs/collection_drop_recreate.js");  // For assertDropAndRecreateCollection.
const dbName = jsTestName();
const collName = "coll";

const queryShapeAggregateFields =
    ["cmdNs", "command", "pipeline", "allowDiskUse", "collation", "let"];

// The outer fields not nested inside queryShape.
const queryStatsAggregateKeyFields = [
    "queryShape",
    "cursor",
    "maxTimeMS",
    "bypassDocumentValidation",
    "comment",
    "apiDeprecationErrors",
    "apiVersion",
    "apiStrict",
    "collectionType",
    "client",
    "hint",
    "readConcern",
    "cursor.batchSize",
];

const testCases = [
    // Default fields.
    {
        pipeline: [{"$changeStream": {}}],
        expectedShapifiedPipeline: [{
            "$changeStream": {
                startAtOperationTime: "?timestamp",
                fullDocument: "default",
                fullDocumentBeforeChange: "off"
            }
        }]
    },
    // Non default field values.
    {
        pipeline: [{
            "$changeStream": {
                fullDocument: "updateLookup",
                fullDocumentBeforeChange: "required",
                showExpandedEvents: true,
            }
        }],
        expectedShapifiedPipeline: [{
            "$changeStream": {
                startAtOperationTime: "?timestamp",
                fullDocument: "updateLookup",
                fullDocumentBeforeChange: "required",
                showExpandedEvents: true,
            }
        }],
    },
    // $changeStream followed by a $match. $changeStream internally creates another $match stage
    // which shouldn't appear in the query shape, but a $match in the user specified pipeline should
    // appear in the query shape.
    {
        pipeline: [{$changeStream: {}}, {$match: {a: "field"}}],
        expectedShapifiedPipeline: [
            {
                "$changeStream": {
                    startAtOperationTime: "?timestamp",
                    fullDocument: "default",
                    fullDocumentBeforeChange: "off"
                }
            },
            {$match: {a: {$eq: "?string"}}}
        ]
    }
];

function assertPipelineField(conn, expectedPipeline) {
    const entry = getLatestQueryStatsEntry(conn, {collName: collName});
    const statsPipeline = getValueAtPath(entry, "key.queryShape.pipeline");
    assert.eq(statsPipeline, expectedPipeline);
}

function validateResumeTokenQueryShape(conn, coll) {
    // Start a change stream.
    const changeStream = coll.watch([]);

    // Going to create an invalid event by checking a change stream on a dropped collection.
    assert.commandWorked(coll.insert({_id: 1}));
    assert(coll.drop());
    assert.soon(() => changeStream.hasNext());
    changeStream.next();
    const invalidateResumeToken = changeStream.getResumeToken();

    // Resume the change stream using 'startAfter' field.
    coll.watch([], {startAfter: invalidateResumeToken});
    assert.commandWorked(coll.insert({_id: 2}));

    const expectedShapifiedPipeline = [{
        "$changeStream": {
            startAfter: {_data: "?string"},
            fullDocument: "default",
            fullDocumentBeforeChange: "off"
        }
    }];
    assertPipelineField(conn, expectedShapifiedPipeline);
}

function validateChangeStreamAggKey(conn) {
    const db = conn.getDB("test");
    assertDropAndRecreateCollection(db, collName);

    // Change streams with 'startAfter' or 'resumeAfter' are only executed after a certain event and
    // require re-parsing a resume token. To validate the query shape of these pipelines, we have to
    // execute the events to register the pipeline.
    validateResumeTokenQueryShape(conn, db[collName]);

    // Validate the key for the rest of the pipelines.
    testCases.forEach(input => {
        const pipeline = input.pipeline;
        const aggCmdObj = {
            aggregate: collName,
            pipeline: pipeline,
            allowDiskUse: false,
            cursor: {batchSize: 2},
            maxTimeMS: 50 * 1000,
            bypassDocumentValidation: false,
            readConcern: {level: "majority"},
            collation: {locale: "en_US", strength: 2},
            hint: {"v": 1},
            comment: "",
            let : {},
            apiDeprecationErrors: false,
            apiVersion: "1",
            apiStrict: false,
        };

        runCommandAndValidateQueryStats({
            coll: db[collName],
            commandName: "aggregate",
            commandObj: aggCmdObj,
            shapeFields: queryShapeAggregateFields,
            keyFields: queryStatsAggregateKeyFields
        });
        assertPipelineField(conn, input.expectedShapifiedPipeline);
    });
}

{
    // Test on a sharded cluster.
    const st = new ShardingTest({
        mongos: 1,
        shards: 2,
        config: 1,
        rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}},
        mongosOptions: {
            setParameter: {
                internalQueryStatsRateLimit: -1,
            }
        },
    });
    validateChangeStreamAggKey(st.s);
    st.stop();
}

{
    // Test the non-sharded case.
    const rst = new ReplSetTest({nodes: 2});
    rst.startSet({setParameter: {internalQueryStatsRateLimit: -1}});
    rst.initiate();
    rst.getPrimary().getDB("admin").setLogLevel(3, "queryStats");

    // Only aggregations run on replica sets have the '$readPreference' field in the key.
    queryStatsAggregateKeyFields.push("$readPreference");
    validateChangeStreamAggKey(rst.getPrimary());
    rst.stopSet();
}
