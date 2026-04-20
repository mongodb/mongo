/**
 * Tests the behavior of explain() when used on a database that does not exist.
 * We should get back an EOF plan
 * @tags: [
 *   # Implicit sharding or timeseries passthroughs, for example, create the collection, directly
 *   # contradicting this test.
 *   assumes_no_implicit_collection_creation_on_get_collection,
 *   # Wrapping in $facet changes the explain output
 *   do_not_wrap_aggregations_in_facets,
 *   # Older versions have different explain behaviour around non-existent DBs
 *   requires_fcv_90,
 * ]
 */
import {before, describe, it} from "jstests/libs/mochalite.js";
import {isEofPlan} from "jstests/libs/query/analyze_plan.js";

const dbName = jsTestName();
const testDB = db.getSiblingDB(dbName);
assert.commandWorked(testDB.dropDatabase({}));

function assertPlanIsEOF(result) {
    assert.commandWorked(result);
    assert(isEofPlan(db, result), "Expected an EOF plan, got: " + tojson(result));
    assert(!db.getMongo().getDBNames().includes(testDB.getName()), "Database should not have been created");

    // listDatabases filters out empty DBs, so we also directly check the config.databases
    // collection to ensure that the database metadata entry was not created.
    const configDB = db.getSiblingDB("config");
    assert.eq(
        null,
        configDB.databases.findOne({_id: testDB.getName()}),
        "Database metadata entry should have been created in config.databases",
    );
}

describe("explain on non-existent database should return an EOF plan", function () {
    before(function () {
        // Every test case starts with dropped DB.
        assert.commandWorked(testDB.dropDatabase({}));
    });

    it("for aggregate", function () {
        assertPlanIsEOF(testDB.test.explain().aggregate([]));
    });

    it("for aggregate specified with explain command", function () {
        assertPlanIsEOF(testDB.test.runCommand({explain: {aggregate: "test", pipeline: [], cursor: {}}}));
    });

    it("for find", function () {
        assertPlanIsEOF(testDB.test.find({}).explain());
    });

    // TODO SERVER-123329: Re-enable these tests once we consistent return EOF for both of these
    // commands, in all configurations.
    // it("for count", function () {
    //     assertPlanIsEOF(testDB.test.explain().count());
    // });

    // it("for distinct", function () {
    //     assertPlanIsEOF(testDB.test.explain().distinct("a"));
    // });

    it("for delete", function () {
        assertPlanIsEOF(testDB.runCommand({explain: {delete: "test", deletes: [{q: {a: 1}, limit: 0}]}}));
    });

    it("for update with upsert:false", function () {
        assertPlanIsEOF(
            testDB.runCommand({explain: {update: "test", updates: [{q: {a: 1}, u: {$set: {b: 1}}, upsert: false}]}}),
        );
    });

    it("for update with upsert:true", function () {
        assertPlanIsEOF(
            testDB.runCommand({explain: {update: "test", updates: [{q: {a: 1}, u: {$set: {b: 1}}, upsert: true}]}}),
        );
    });

    it("for bulkWrite", function () {
        const res = testDB.runCommand({
            explain: {
                bulkWrite: 1,
                ops: [{update: 0, filter: {a: 1}, updateMods: {$set: {b: 1}}}],
                nsInfo: [{ns: `${dbName}.test`}],
            },
        });
        // If auth is enabled, the bulkWrite command will fail with an authorization error before
        // it can return an EOF plan.
        if (TestData.auth) {
            assert.commandFailed(res);
        } else {
            assertPlanIsEOF(res);
        }
    });

    it("for rawData aggregate", function () {
        assertPlanIsEOF(testDB.test.explain("executionStats").aggregate([], {rawData: true}));
    });

    it("for rawData delete", function () {
        assertPlanIsEOF(
            testDB.runCommand({explain: {delete: "test", deletes: [{q: {a: 1}, limit: 0}], rawData: true}}),
        );
    });

    // Find And Modify is unique. Unlike other commands, it will throw an error if the database
    // does not exist, instead of returning an EOF plan. TODO SERVER-123329: Update these assertions
    // once we always return EOF rather than erroring here.
    it("for findAndModify with upsert:false", function () {
        assert.commandFailedWithCode(
            testDB.runCommand({
                explain: {findAndModify: "test", query: {a: 1}, update: {$set: {b: 1}}, upsert: false},
            }),
            ErrorCodes.NamespaceNotFound,
        );
    });

    it("for findAndModify with upsert:true", function () {
        assert.commandFailedWithCode(
            testDB.runCommand({
                explain: {findAndModify: "test", query: {a: 1}, update: {$set: {b: 1}}, upsert: true},
            }),
            ErrorCodes.NamespaceNotFound,
        );
    });
});
