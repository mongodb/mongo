/**
 * Tests for the validation of $merge 'whenMatched' pipelines, with and without query stats enabled,
 * on both standalone and sharded deployments.
 *
 * This case has a known inconsistency:
 * When query stats is enabled (rate limit -1), the aggregation always fails with
 * ErrorCodes.QueryStatsFailedToRecord because query stats attempts to serialize the query shape at
 * registration time, and the invalid pipeline causes that to fail.
 *
 * When query stats is not enabled (rate limit 0), the expected behavior depends on topology:
 * - On standalone, the empty collection case does not throw, and after inserting a document
 *   the aggregation fails with error code 40272.
 * - On sharded clusters (through mongos), the serialize() codepath is invoked earlier, so even
 *   the empty collection case fails with 40272.
 *
 * This inconsistency makes it problematic to include in test suites with passthroughs. We also want
 * to preserve the coverage of what happens to these $merge pipelines if query stats is not enabled
 * - since they get a bit further in execution before failing. The aim is to enable query stats by
 * default in all our test suites, so this behavior is moved out to noPassthrough to test here.
 *
 */
import {assertErrorCode} from "jstests/aggregation/extras/utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {describe, it} from "jstests/libs/mochalite.js";

function runTest(testDB, {queryStatsEnabled, isMongos, customSetupFn = () => {}}) {
    // Ensure the database exists - just insert into some random collection.
    assert.commandWorked(testDB.dropDatabase());
    assert.commandWorked(testDB.be_sure_it_exists.insertOne({_id: 0}));

    const source = testDB[`${jsTestName()}_source`];
    const target = testDB[`${jsTestName()}_target`];
    [source, target].forEach((coll) => coll.drop());

    customSetupFn(source);

    const problematicMergePipeline = [
        {$merge: {into: target.getName(), whenMatched: [{$addFields: 2}], whenNotMatched: "insert"}},
    ];
    const addFieldsErrorCode = 40272;

    if (queryStatsEnabled) {
        // With query stats enabled, the aggregation always fails with QueryStatsFailedToRecord,
        // regardless of topology or collection contents.
        assertErrorCode(source, problematicMergePipeline, ErrorCodes.QueryStatsFailedToRecord);

        assert.commandWorked(source.insert({_id: 0}));
        assertErrorCode(source, problematicMergePipeline, ErrorCodes.QueryStatsFailedToRecord);
    } else if (isMongos) {
        // Sharded clusters hit the error earlier since they invoke the serialize() codepath,
        // so even the empty collection case fails with the addFields error code.
        assertErrorCode(source, problematicMergePipeline, addFieldsErrorCode);

        assert.commandWorked(source.insert({_id: 0}));
        assertErrorCode(source, problematicMergePipeline, addFieldsErrorCode);
    } else {
        // Standalone without query stats: the empty collection case does not throw with the
        // addFields error code, since the $merge pipeline is never executed.
        assert.doesNotThrow(() => source.aggregate(problematicMergePipeline));

        assert.commandWorked(source.insert({_id: 0}));
        assertErrorCode(source, problematicMergePipeline, addFieldsErrorCode);
    }
}

/**
 * Tests that $setField/$unsetField with null chars in the 'field' argument correctly fail when used
 * inside a $merge 'whenMatched' pipeline. With query stats enabled, the error is
 * QueryStatsFailedToRecord; without, the original validation error codes are expected.
 *
 * TODO SERVER-96515: Move this assertion back to
 * jstests/aggregation/expressions/expression_set_field_null_chars.js once query stats no longer
 * interferes with $merge pipeline validation.
 */
function runSetFieldNullCharsMergeTest(testDB, {queryStatsEnabled}) {
    const coll = testDB[`${jsTestName()}_setField`];
    coll.drop();
    assert.commandWorked(coll.insert({_id: 1, foo: "bar"}));

    function assertMergeWithSetFieldFails({field, codes}) {
        const setFieldExpression = {$setField: {field, input: {}, value: true}};
        const unsetFieldExpression = {$unsetField: {field, input: {}}};
        function useWithinMergePipeline(expression) {
            return [
                {
                    $merge: {
                        into: coll.getName(),
                        whenMatched: [{$replaceWith: expression}],
                        whenNotMatched: "discard",
                    },
                },
            ];
        }

        const expectedCodes = queryStatsEnabled ? [...codes, ErrorCodes.QueryStatsFailedToRecord] : codes;

        assertErrorCode(coll, useWithinMergePipeline(setFieldExpression), expectedCodes);
        assertErrorCode(coll, useWithinMergePipeline(unsetFieldExpression), expectedCodes);
    }

    // A select and brief subset of invalid field names (the full set is tested in
    // expression_set_field_null_chars.js).
    assertMergeWithSetFieldFails({field: "\x00a", codes: [9534700, 9423101]});
    assertMergeWithSetFieldFails({field: "$a\x00", codes: [16411, 9423101]});
    assertMergeWithSetFieldFails({field: {$concat: ["a\x00"]}, codes: [4161106, 9423101]});
}

const optionsToEnableQueryStats = {
    setParameter: {internalQueryStatsRateLimit: -1, internalQueryStatsErrorsAreCommandFatal: true},
};
//
// Standalone tests.
//

describe("merge_pipeline_validation", function testMergePipelineValidation() {
    describe("Standalone", function testStandalone() {
        it("should work without query stats", function testStandaloneWithoutQueryStats() {
            const conn = MongoRunner.runMongod({
                setParameter: {internalQueryStatsRateLimit: 0},
            });
            const testDB = conn.getDB("test");

            runTest(testDB, {queryStatsEnabled: false, isMongos: false});
            runSetFieldNullCharsMergeTest(testDB, {queryStatsEnabled: false});

            MongoRunner.stopMongod(conn);
        });

        it("should work with query stats enabled", function testStandaloneWithQueryStatsEnabled() {
            const conn = MongoRunner.runMongod(optionsToEnableQueryStats);
            const testDB = conn.getDB("test");

            runTest(testDB, {queryStatsEnabled: true, isMongos: false});
            runSetFieldNullCharsMergeTest(testDB, {queryStatsEnabled: true});

            MongoRunner.stopMongod(conn);
        });
    });

    describe("Sharded Cluster", function testShardedCluster() {
        it("should work without query stats (unsharded and sharded collection)", function testShardedWithoutQueryStats() {
            const st = new ShardingTest({shards: 2});
            const testDB = st.s.getDB("test");

            runTest(testDB, {queryStatsEnabled: false, isMongos: true});

            runTest(testDB, {
                queryStatsEnabled: false,
                isMongos: true,
                customSetupFn: (sourceColl) => {
                    st.shardColl(sourceColl, {_id: 1}, {_id: 1});
                },
            });

            runSetFieldNullCharsMergeTest(testDB, {queryStatsEnabled: false});

            st.stop();
        });

        it("should work with query stats enabled (unsharded and sharded collection)", function testShardedWithQueryStatsEnabled() {
            const st = new ShardingTest({
                shards: 2,
                mongosOptions: optionsToEnableQueryStats,
                rsOptions: optionsToEnableQueryStats,
            });
            const testDB = st.s.getDB("test");

            runTest(testDB, {queryStatsEnabled: true, isMongos: true});

            runTest(testDB, {
                queryStatsEnabled: true,
                isMongos: true,
                customSetupFn: (sourceColl) => {
                    st.shardColl(sourceColl, {_id: 1}, {_id: 1});
                },
            });

            runSetFieldNullCharsMergeTest(testDB, {queryStatsEnabled: true});

            st.stop();
        });
    });
});
