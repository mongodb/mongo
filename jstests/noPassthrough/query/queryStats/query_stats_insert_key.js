/**
 * This test confirms that query stats store key fields for an insert command are properly nested
 * and none are missing.
 *
 * @tags: [featureFlagQueryStatsInsert]
 */
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {
    getQueryStatsInsertCmd,
    insertKeyFieldsComplex,
    insertKeyFieldsRequired,
    queryShapeInsertFieldsRequired,
    resetQueryStatsStore,
    runCommandAndValidateQueryStats,
} from "jstests/libs/query/query_stats_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const collName = jsTestName();

/**
 * Runs the test suite against a specific topology.
 *
 * @param {string} topologyName - Name of the topology (e.g. "Standalone", "Sharded")
 * @param {Function} setupFn - Returns {fixture, testDB}
 * @param {Function} teardownFn - Takes {fixture} and cleans up
 */
function runInsertKeyTests(topologyName, setupFn, teardownFn) {
    describe(`query stats insert key (${topologyName})`, function () {
        let fixture;
        let testDB;
        let coll;

        before(function () {
            const setupRes = setupFn();
            fixture = setupRes.fixture;
            testDB = setupRes.testDB;
            coll = testDB[collName];
        });

        after(function () {
            if (fixture) {
                teardownFn(fixture);
            }
        });

        beforeEach(function () {
            coll.drop();
            resetQueryStatsStore(testDB.getMongo(), "1MB");
        });

        it("should validate simple insert key fields", function () {
            const insertCommandObjRequired = {
                insert: collName,
                documents: [{v: 1}],
            };

            runCommandAndValidateQueryStats({
                coll: coll,
                commandName: "insert",
                commandObj: insertCommandObjRequired,
                shapeFields: queryShapeInsertFieldsRequired,
                keyFields: insertKeyFieldsRequired,
                checkExplain: false,
            });
        });

        it("should validate complex insert key fields", function () {
            const insertCommandObjComplex = {
                insert: collName,
                documents: [{v: 1, name: "Alice", score: 99.5}],
                ordered: false,
                bypassDocumentValidation: true,
                comment: "complex insert test!!!",
                maxTimeMS: 50 * 1000,
                apiDeprecationErrors: false,
                apiVersion: "1",
                apiStrict: false,
                $readPreference: {mode: "primary"},
            };

            runCommandAndValidateQueryStats({
                coll: coll,
                commandName: "insert",
                commandObj: insertCommandObjComplex,
                shapeFields: queryShapeInsertFieldsRequired,
                keyFields: insertKeyFieldsComplex,
                checkExplain: false,
            });
        });

        it("should produce same shape for inserts with different document contents", function () {
            // Inserts with different document shapes should produce the same query stats entry
            // because 'documents' is always shapified as '?array<?object>'.
            assert.commandWorked(testDB.runCommand({insert: collName, documents: [{v: 1}]}));
            assert.commandWorked(
                testDB.runCommand({
                    insert: collName,
                    documents: [{name: "Alice", score: 42, tags: ["a"]}],
                }),
            );

            const entries = getQueryStatsInsertCmd(testDB.getMongo(), {collName: coll.getName()});
            assert.eq(entries.length, 1, "Both inserts should share the same query shape: " + tojson(entries));
            assert.eq(entries[0].key.queryShape.documents, "?array<?object>");
        });

        it("transformIdentifiers is a no-op on insert-specific shape fields", function () {
            // Insert shapes contain no user identifier strings — 'documents' always shapifies to
            // '?array<?object>' without introspecting field names, and there are no q/u/c fields
            // like update has. So transformIdentifiers has nothing to hash within insert-specific
            // shape fields. The values that do appear (command name, documents placeholder, and
            // comment) are all shapified, which happens regardless of the flag.
            assert.commandWorked(
                testDB.runCommand({
                    insert: collName,
                    documents: [{v: 1, name: "Alice"}],
                    comment: "transformIdentifiers no-op check",
                }),
            );

            const plain = getQueryStatsInsertCmd(testDB.getMongo(), {collName: coll.getName()});
            const tokenized = getQueryStatsInsertCmd(testDB.getMongo(), {transformIdentifiers: true});

            assert.eq(plain.length, 1);
            assert.eq(tokenized.length, 1);
            assert.eq(plain[0].key.queryShape.command, tokenized[0].key.queryShape.command);
            assert.eq(plain[0].key.queryShape.documents, tokenized[0].key.queryShape.documents);
            assert.eq(plain[0].key.comment, tokenized[0].key.comment);
        });
    });
}

runInsertKeyTests(
    "Standalone",
    () => {
        const conn = MongoRunner.runMongod({
            setParameter: {internalQueryStatsWriteCmdSampleRate: 1},
        });
        const testDB = conn.getDB("test");
        testDB[collName].drop();
        return {fixture: conn, testDB};
    },
    (fixture) => MongoRunner.stopMongod(fixture),
);

// TODO SERVER-122076: Enable the Sharded variant once query stats collection for inserts is wired
// through mongos and the sharded write path. The current branch only supports standalone.
/*
runInsertKeyTests(
    "Sharded",
    () => {
        const st = new ShardingTest({
            shards: 2,
            mongosOptions: {
                setParameter: {internalQueryStatsWriteCmdSampleRate: 1},
            },
        });
        const testDB = st.s.getDB("test");
        st.shardColl(testDB[collName], {_id: 1}, {_id: 0});
        return {fixture: st, testDB};
    },
    (st) => st.stop(),
);
*/
