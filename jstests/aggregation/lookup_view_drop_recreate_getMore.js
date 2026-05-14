/**
 * SERVER-121988: $lookup and $graphLookup against a view that is dropped, recreated, or swapped
 * mid-cursor (across a getMore boundary) must not silently return zero matches. The fix pins the
 * resolved view definition into the cursor's plan context at cursor-open time, so subsequent
 * getMore batches use the same view definition that the initial batch saw.
 *
 * This test exercises three mutation patterns -- drop, drop+recreate (same backing, new pipeline),
 * and view-to-collection swap (view points at a different backing collection) -- in each case
 * across at least one getMore boundary, for both $lookup and $graphLookup.
 *
 * @tags: [
 *   # The fix is observational: this test asserts on the document count returned by the cursor
 *   # before and after the catalog mutation. Implicit collection creation must be disabled so the
 *   # `drop' step really drops.
 *   assumes_no_implicit_collection_creation_after_drop,
 *   # The cursor must remain open across at least one getMore. Wrapping in $facet collapses
 *   # the pipeline to a single command boundary and defeats the test.
 *   do_not_wrap_aggregations_in_facets,
 *   # The test directly drops + recreates a foreign view between getMore calls. Causal consistency
 *   # would either replay the drop into the subsequent getMore or report stale-snapshot errors,
 *   # both of which would mask the bug we are exercising.
 *   does_not_support_causal_consistency,
 *   # Stepdowns between getMore calls invalidate the cursor and produce ResumableScanError before
 *   # the catalog mutation can race the resolver -- which is a different code path.
 *   does_not_support_stepdowns,
 *   # The test asserts on resolved view behaviour, and is therefore sensitive to anything that
 *   # bypasses view resolution.
 *   requires_fcv_81,
 * ]
 */

const testDB = db.getSiblingDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());

const sourceCollName = "source";
const backingAName = "backing_a";
const backingBName = "backing_b";
const viewName = "foreign_view";

const source = testDB[sourceCollName];
const backingA = testDB[backingAName];
const backingB = testDB[backingBName];

// Number of source documents must exceed the initial-batch size so that at least one getMore is
// required to drain the cursor. The aggregation default batch size is 101; we use 200 docs and
// pin a smaller cursor batch size (5) below to make this robust against future default changes.
const N_SOURCE = 200;
const CURSOR_BATCH = 5;

function reseed() {
    source.drop();
    backingA.drop();
    backingB.drop();
    // `drop' is idempotent on a non-existent namespace, but mongos returns NamespaceNotFound.
    // We swallow that and only that.
    const dropRes = testDB.runCommand({drop: viewName});
    if (!dropRes.ok && dropRes.code !== ErrorCodes.NamespaceNotFound) {
        assert.commandWorked(dropRes);
    }

    // Source: _id 0..N-1 join_field cycling 1..3.
    const sourceDocs = [];
    for (let i = 0; i < N_SOURCE; i++) {
        sourceDocs.push({_id: i, join_field: (i % 3) + 1});
    }
    assert.commandWorked(source.insertMany(sourceDocs, {ordered: false}));

    // Backing A: three matching documents.
    assert.commandWorked(backingA.insertMany([
        {_id: "a1", key: 1, payload: "from_A"},
        {_id: "a2", key: 2, payload: "from_A"},
        {_id: "a3", key: 3, payload: "from_A"},
    ]));

    // Backing B: three matching documents with different payload (so we can detect a silent swap).
    assert.commandWorked(backingB.insertMany([
        {_id: "b1", key: 1, payload: "from_B"},
        {_id: "b2", key: 2, payload: "from_B"},
        {_id: "b3", key: 3, payload: "from_B"},
    ]));

    // Create the view targeting backingA initially.
    assert.commandWorked(
        testDB.runCommand({create: viewName, viewOn: backingAName, pipeline: []}));
}

// Drains a cursor in batches, optionally invoking `mutateFn' exactly once after the first getMore.
// Returns the full result array.
function drainWithMutation(cursorReply, mutateFn) {
    const collName = cursorReply.cursor.ns.split(".").slice(1).join(".");
    let docs = cursorReply.cursor.firstBatch.slice();
    let cursorId = cursorReply.cursor.id;
    let mutated = false;

    while (cursorId.toString() !== "NumberLong(0)" && cursorId !== 0 &&
           (typeof cursorId.compare === "function" ? cursorId.compare(0) !== 0 : cursorId !== 0)) {
        const getMore = assert.commandWorked(testDB.runCommand({
            getMore: cursorId,
            collection: collName,
            batchSize: CURSOR_BATCH,
        }));
        docs = docs.concat(getMore.cursor.nextBatch);
        cursorId = getMore.cursor.id;

        // Mutate exactly once after the first getMore so the *next* getMore (and any cursor
        // re-resolution path triggered by it) sees a mutated catalog. This is the SERVER-121988
        // race window.
        if (!mutated && mutateFn !== null && cursorId.toString() !== "NumberLong(0)") {
            mutateFn();
            mutated = true;
        }
    }
    return docs;
}

// Runs a $lookup pipeline that joins source.join_field == foreign.key against the view, captures
// the full expected result with no mutation, then re-runs the same pipeline with the supplied
// mutation function fired between getMore batches. The two result sets must be byte-for-byte
// equal -- the cursor must stick to the view definition it opened against.
function runLookupCase({caseName, mutateFn}) {
    jsTest.log(`---- $lookup case: ${caseName} ----`);
    reseed();

    const pipeline = [
        {$sort: {_id: 1}},
        {
            $lookup: {
                from: viewName,
                localField: "join_field",
                foreignField: "key",
                as: "matches",
            },
        },
        {$project: {_id: 1, payloads: "$matches.payload"}},
    ];

    // Baseline: drain without mutation.
    const baselineCursor = assert.commandWorked(testDB.runCommand({
        aggregate: sourceCollName,
        pipeline: pipeline,
        cursor: {batchSize: CURSOR_BATCH},
    }));
    const baselineDocs = drainWithMutation(baselineCursor, null);
    assert.eq(baselineDocs.length, N_SOURCE, "baseline must return all source documents");
    for (const d of baselineDocs) {
        assert.eq(d.payloads.length, 1, () => `baseline payloads malformed: ${tojson(d)}`);
        assert.eq(d.payloads[0], "from_A", () => `baseline must see backing_a: ${tojson(d)}`);
    }

    // Re-run, mutating the catalog between getMores.
    reseed();
    const racedCursor = assert.commandWorked(testDB.runCommand({
        aggregate: sourceCollName,
        pipeline: pipeline,
        cursor: {batchSize: CURSOR_BATCH},
    }));
    const racedDocs = drainWithMutation(racedCursor, mutateFn);

    // Invariant: the cursor must continue to resolve against the view definition it opened
    // with. Either (a) the cursor returns the same N docs all with payload from_A, or (b) the
    // server explicitly errors a getMore call. Silently returning empty matches for the
    // remaining batches is the SERVER-121988 bug and is forbidden.
    assert.eq(racedDocs.length, N_SOURCE,
        `cursor must return ${N_SOURCE} docs even after catalog mutation; got ${racedDocs.length}`);

    let postMutationEmptyCount = 0;
    for (const d of racedDocs) {
        if (!Array.isArray(d.payloads) || d.payloads.length === 0) {
            postMutationEmptyCount++;
        } else {
            // If the cursor saw any matches, they must be from the *original* view (backingA).
            // A leaked mutation would show payload from_B (swap case) or no payloads (drop case).
            assert.eq(d.payloads[0], "from_A",
                `cursor returned a payload from a mutated view -- SERVER-121988 regression: ${tojson(d)}`);
        }
    }
    assert.eq(postMutationEmptyCount, 0,
        `cursor returned ${postMutationEmptyCount} silently-empty join rows -- SERVER-121988 regression`);

    jsTest.log(`OK: ${caseName} -- cursor stayed bound to its opened view definition.`);
}

// Same as runLookupCase but for $graphLookup. The recursive $graphLookup path resolves the
// foreign view at depth=0 and then re-resolves once per BFS level on the shard. Mid-pipeline
// catalog mutation must not change which view those resolutions hit.
function runGraphLookupCase({caseName, mutateFn}) {
    jsTest.log(`---- $graphLookup case: ${caseName} ----`);
    reseed();

    const pipeline = [
        {$sort: {_id: 1}},
        {
            $graphLookup: {
                from: viewName,
                startWith: "$join_field",
                connectFromField: "key",
                connectToField: "key",
                as: "matches",
                maxDepth: 1,
            },
        },
        {$project: {_id: 1, payloads: "$matches.payload"}},
    ];

    const baselineCursor = assert.commandWorked(testDB.runCommand({
        aggregate: sourceCollName,
        pipeline: pipeline,
        cursor: {batchSize: CURSOR_BATCH},
    }));
    const baselineDocs = drainWithMutation(baselineCursor, null);
    assert.eq(baselineDocs.length, N_SOURCE);
    for (const d of baselineDocs) {
        assert(Array.isArray(d.payloads) && d.payloads.length >= 1,
            `baseline $graphLookup row must have at least one match: ${tojson(d)}`);
        for (const p of d.payloads) {
            assert.eq(p, "from_A", `baseline must see only backing_a: ${tojson(d)}`);
        }
    }

    reseed();
    const racedCursor = assert.commandWorked(testDB.runCommand({
        aggregate: sourceCollName,
        pipeline: pipeline,
        cursor: {batchSize: CURSOR_BATCH},
    }));
    const racedDocs = drainWithMutation(racedCursor, mutateFn);
    assert.eq(racedDocs.length, N_SOURCE,
        `$graphLookup cursor must return ${N_SOURCE} docs; got ${racedDocs.length}`);

    let postMutationEmptyCount = 0;
    for (const d of racedDocs) {
        if (!Array.isArray(d.payloads) || d.payloads.length === 0) {
            postMutationEmptyCount++;
        } else {
            for (const p of d.payloads) {
                assert.eq(p, "from_A",
                    `$graphLookup leaked a payload from a mutated view -- SERVER-121988 regression: ${tojson(d)}`);
            }
        }
    }
    assert.eq(postMutationEmptyCount, 0,
        `$graphLookup cursor silently returned ${postMutationEmptyCount} empty rows after catalog mutation -- SERVER-121988 regression`);

    jsTest.log(`OK: ${caseName} -- $graphLookup stayed bound to its opened view definition.`);
}

// ---- Mutation fixtures ----

// Case A: drop the view outright between getMore batches.
function mutateDrop() {
    assert.commandWorked(testDB.runCommand({drop: viewName}));
}

// Case B: drop + recreate the view with the same backing collection (i.e. functionally
// equivalent pipeline). This is the classic "drop+recreate" race.
function mutateDropRecreateSameBacking() {
    assert.commandWorked(testDB.runCommand({drop: viewName}));
    assert.commandWorked(
        testDB.runCommand({create: viewName, viewOn: backingAName, pipeline: []}));
}

// Case C: drop + recreate the view pointing at a *different* backing collection. This is the
// swap case -- the user-visible payload would change if the bug leaks through.
function mutateSwapToBackingB() {
    assert.commandWorked(testDB.runCommand({drop: viewName}));
    assert.commandWorked(
        testDB.runCommand({create: viewName, viewOn: backingBName, pipeline: []}));
}

// Case D: view-to-collection swap. Drop the view and replace it with a real collection of
// the same name. Mongos must not start re-routing $lookup to the collection's data.
function mutateViewToCollectionSwap() {
    assert.commandWorked(testDB.runCommand({drop: viewName}));
    assert.commandWorked(testDB[viewName].insertMany([
        {_id: "c1", key: 1, payload: "from_C_collection"},
        {_id: "c2", key: 2, payload: "from_C_collection"},
        {_id: "c3", key: 3, payload: "from_C_collection"},
    ]));
}

// ---- Run the matrix. ----

runLookupCase({caseName: "drop", mutateFn: mutateDrop});
runLookupCase({caseName: "drop+recreate same backing", mutateFn: mutateDropRecreateSameBacking});
runLookupCase({caseName: "swap to backing_b", mutateFn: mutateSwapToBackingB});
runLookupCase({caseName: "view->collection swap", mutateFn: mutateViewToCollectionSwap});

runGraphLookupCase({caseName: "drop", mutateFn: mutateDrop});
runGraphLookupCase({caseName: "drop+recreate same backing", mutateFn: mutateDropRecreateSameBacking});
runGraphLookupCase({caseName: "swap to backing_b", mutateFn: mutateSwapToBackingB});
runGraphLookupCase({caseName: "view->collection swap", mutateFn: mutateViewToCollectionSwap});

jsTest.log("SERVER-121988 regression suite passed.");
