/**
 * A view definition stored by an older binary can contain an internal-only stage, since older
 * versions did not check stage client types when validating view definitions. Verifies that the
 * current binary rejects such definitions when the view catalog loads them, so the internal stage
 * never executes.
 *
 * Procedure:
 * - Start mongod with the last-lts version.
 * - Insert into a collection, create a view containing an internal-only stage, and create a view
 *   whose argument data is merely shaped like an internal stage.
 * - Restart mongod with the latest version on the same data files.
 * - Reading through the internal-only view must fail with InvalidViewDefinition, so the internal
 *   stage never runs.
 * - The literal-data view must still resolve and read successfully.
 *
 * requires_persistence because this test restarts mongod and expects the stored view definitions
 * to survive on the data files.
 *
 * @tags: [requires_persistence]
 */

const dbpath = MongoRunner.dataPath + "views_internal_only_stage_reload";
resetDbpath(dbpath);

const defaultOptions = {
    dbpath: dbpath,
    noCleanData: true,
};

const mongodOptionsLastLTS = Object.extend({binVersion: "last-lts"}, defaultOptions);
const mongodOptionsCurrent = Object.extend({binVersion: "latest"}, defaultOptions);

const kInternalStage = {
    $setVariableFromSubPipeline: {
        setVariable: "$$SEARCH_META",
        pipeline: [{$collStats: {count: {}}}],
    },
};

// Start on the old version, which stores view definitions containing internal-only stages.
let conn = MongoRunner.runMongod(mongodOptionsLastLTS);
assert.neq(null, conn, "mongod was unable to start with version " + tojson(mongodOptionsLastLTS));

let testDb = conn.getDB(jsTestName());
assert.commandWorked(testDb.coll.insert({_id: 1, a: 1}));
// Create the clean views first: once the poisoned definition is stored, the old binary's view
// catalog is in an invalid state and rejects further view creation.
assert.commandWorked(
    testDb.createView("literalView", "coll", [{$project: {value: {$literal: kInternalStage}}}]),
);
// A plain view is also needed so the system.views collection exists for the applyOps write below.
assert.commandWorked(testDb.createView("plainView", "coll", [{$match: {a: 1}}]));

// Store the internal-only view definition directly into system.views via applyOps, the way an
// internal path (e.g. a mongos-forwarded create) would store it: the per-stage user-request gate
// on the old binary would reject the same definition arriving through 'create' from a user
// connection, but it is not consulted for this direct write. The record's _id must be the fully
// qualified namespace of the view.
assert.commandWorked(
    testDb.adminCommand({
        applyOps: [
            {
                op: "i",
                ns: testDb.getName() + ".system.views",
                o: {
                    _id: testDb.getName() + ".poisonedView",
                    viewOn: "coll",
                    pipeline: [kInternalStage],
                },
            },
        ],
    }),
);

// The poisoned view is intentionally invalid, so the validate-on-shutdown override would fail
// on every shutdown of this test. Skip it.
TestData.skipCollectionAndIndexValidation = true;

MongoRunner.stopMongod(conn);

// Restart with the latest binary on the old version's data files.
conn = MongoRunner.runMongod(mongodOptionsCurrent);
assert.neq(null, conn, "mongod was unable to start with version " + tojson(mongodOptionsCurrent));
testDb = conn.getDB(jsTestName());

// The stored internal-only stage must not be resolvable: the view is rejected as the view catalog
// loads, so reads through it fail with InvalidViewDefinition rather than running the internal
// stage.
assert.commandFailedWithCode(
    testDb.runCommand({find: "poisonedView"}),
    ErrorCodes.InvalidViewDefinition,
);
assert.commandFailedWithCode(
    testDb.runCommand({aggregate: "poisonedView", pipeline: [{$count: "n"}], cursor: {}}),
    ErrorCodes.InvalidViewDefinition,
);

// The view whose argument data is shaped like an internal stage is not itself internal, so it must
// remain readable.
assert.eq(testDb.literalView.find().toArray(), [{_id: 1, value: kInternalStage}], {
    view: "literalView",
});

MongoRunner.stopMongod(conn);
