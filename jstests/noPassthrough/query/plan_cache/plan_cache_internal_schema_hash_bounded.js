/**
 * Regression test that exercises the plan-cache hash for deeply-nested $jsonSchema queries.
 *
 * Background: the MatchExpressionHashVisitor implementations for several InternalSchema*
 * MatchExpression nodes used to re-enter the tree walker for each child via an inner
 * `combine(MatchExpressionHasher{}(child))` call. Because the outer tree_walker::walk() was
 * already descending into the same subtree, every InternalSchema* node effectively doubled
 * the work below it, turning hashing of a query solution into an O(2^n) operation in the
 * depth of nested InternalSchema* nodes.
 *
 * In a 16-level-deep $jsonSchema this empirically pushed plan-cache hashing into the
 * many-minute range, fully occupying a CPU core and (because plan enumeration does not
 * check interrupt) rendering the operation unresponsive to killOp(). This is a DoS surface
 * on any tenant with read access against a collection.
 *
 * This test pins the bound: a 30-level-deep $jsonSchema find against an empty collection
 * must complete within a few seconds of wall-clock time. We use the `maxTimeMS` budget on
 * the command itself to bound the operation server-side, and we additionally bound the
 * wall-clock of the round trip on the client side to catch regressions if interrupt
 * checks are added but the underlying complexity is not fixed.
 *
 * Asserts: the command MUST NOT fail with MaxTimeMSExpired. With the exponential bug it
 * would either trip the budget (if interrupt is now checked) or wedge the planner for
 * minutes (if it is not). With the linear fix it returns essentially instantly.
 *
 * @tags: [
 *   requires_fcv_82,
 * ]
 */
const conn = MongoRunner.runMongod({});
assert.neq(null, conn, "mongod was unable to start up");

const db = conn.getDB("plan_cache_internal_schema_hash_bounded");
const coll = db.coll;
coll.drop();

// Empty collection on purpose. The Jira description for this fix is explicit that the
// underlying documents do not matter: the planner spends its time hashing the query
// solution, not evaluating it. Keeping the collection empty rules out any chance that
// per-document evaluation cost masks a planner regression.

/**
 * Build a $jsonSchema body with `depth` levels of nested `properties: { x: { ... } }`
 * wrapped around a small leaf predicate. This produces a chain of nested
 * InternalSchemaObjectMatchExpression / InternalSchemaAllowedPropertiesMatchExpression
 * nodes in the parsed MatchExpression tree, which is what the exponential bug walks.
 */
function buildNestedSchema(depth) {
    let schema = {type: "string"};
    for (let i = 0; i < depth; ++i) {
        schema = {
            type: "object",
            properties: {x: schema},
        };
    }
    return schema;
}

// 30 levels is deep enough to make the old O(2^n) path obviously wedge (16 levels was
// already 6-8 minutes in the original repro). Keep some headroom under the BSON / parser
// depth limits, which sit well above 30.
const NESTED_DEPTH = 30;

// 5 seconds is the time budget the planner has to finish. The bug-free path hashes the
// MatchExpression tree linearly in O(n) nodes (a handful of microseconds for 30 levels),
// so this is generous. The exponential path on 30 levels would need 2^30 work units,
// which is many minutes even at modern CPU speeds.
const TIME_BUDGET_MS = 5 * 1000;

const filter = {$jsonSchema: buildNestedSchema(NESTED_DEPTH)};

// Sanity check: confirm the filter parses on its own before we wire it into a timed
// command. If parse itself blows up that is a different bug class than what this test
// guards.
{
    const parseCheck = db.runCommand({
        find: coll.getName(),
        filter: filter,
        limit: 1,
    });
    assert.commandWorked(parseCheck, "deeply-nested $jsonSchema must at least parse");
}

/**
 * Run `cmd` under maxTimeMS=TIME_BUDGET_MS and assert it completes inside the budget,
 * and not by being killed for exceeding it. Returns the wall-clock duration in ms so
 * callers can also assert on client-observed latency.
 */
function runUnderBudget(cmd, label) {
    const cmdWithBudget = Object.merge(cmd, {maxTimeMS: TIME_BUDGET_MS});
    const startMs = Date.now();
    const res = db.runCommand(cmdWithBudget);
    const elapsedMs = Date.now() - startMs;

    assert.neq(
        res.code,
        ErrorCodes.MaxTimeMSExpired,
        () =>
            "plan-cache hashing for " +
            label +
            " exceeded the " +
            TIME_BUDGET_MS +
            "ms budget (elapsed " +
            elapsedMs +
            "ms). This is the regression signature for SERVER-125872 " +
            "(exponential hashing of InternalSchema* match expressions). " +
            "Response: " +
            tojson(res),
    );
    assert.commandWorked(res, label);

    // Defensive: even within the maxTimeMS budget, if hashing went quadratic instead of
    // linear we would still see wall-clock blow past the budget on the round trip
    // because the planner runs before any cursor batch is returned. Fail loud.
    assert.lt(
        elapsedMs,
        TIME_BUDGET_MS,
        () =>
            "wall-clock for " +
            label +
            " (" +
            elapsedMs +
            "ms) exceeded the " +
            TIME_BUDGET_MS +
            "ms budget despite no MaxTimeMSExpired. Suggests the operation completed " +
            "outside the interrupt check window — still a SERVER-125872 regression.",
    );

    return elapsedMs;
}

// 1) explain → exercises the plan-enumeration + plan-cache hash path without paying
//    execution cost. This is the path the original repro lit up.
{
    const elapsedMs = runUnderBudget(
        {explain: {find: coll.getName(), filter: filter}, verbosity: "queryPlanner"},
        "explain(find, nested $jsonSchema, depth=" + NESTED_DEPTH + ")",
    );
    jsTest.log("explain of depth-" + NESTED_DEPTH + " $jsonSchema completed in " + elapsedMs + "ms");
}

// 2) find → end-to-end path, including any plan-cache key insertion that the explain
//    above might have bypassed.
{
    const elapsedMs = runUnderBudget(
        {find: coll.getName(), filter: filter, limit: 1},
        "find(nested $jsonSchema, depth=" + NESTED_DEPTH + ")",
    );
    jsTest.log("find on depth-" + NESTED_DEPTH + " $jsonSchema completed in " + elapsedMs + "ms");
}

// 3) Re-run to exercise the warm-cache path. With the bug, hashing is repeated on each
//    planning attempt, so even a warm-cache miss is exponential. With the fix, both
//    runs are sub-millisecond.
{
    const elapsedMs = runUnderBudget(
        {find: coll.getName(), filter: filter, limit: 1},
        "find(nested $jsonSchema, depth=" + NESTED_DEPTH + ") — second run",
    );
    jsTest.log("second find on depth-" + NESTED_DEPTH + " $jsonSchema completed in " + elapsedMs + "ms");
}

// 4) An $internalSchemaAllElemMatchFromIndex chain reproduces the same exponential class
//    on a different InternalSchema* visitor; pin that path too so a fix that only
//    addresses InternalSchemaObjectMatchExpression cannot silently regress the
//    AllElemMatchFromIndex path.
{
    let allElemFilter = {a: {$lt: 5}};
    for (let i = 0; i < NESTED_DEPTH; ++i) {
        allElemFilter = {"a.b": {$_internalSchemaAllElemMatchFromIndex: [2, allElemFilter]}};
    }

    const elapsedMs = runUnderBudget(
        {find: coll.getName(), filter: allElemFilter, limit: 1},
        "find($_internalSchemaAllElemMatchFromIndex chain, depth=" + NESTED_DEPTH + ")",
    );
    jsTest.log(
        "find on depth-" +
            NESTED_DEPTH +
            " $_internalSchemaAllElemMatchFromIndex chain completed in " +
            elapsedMs +
            "ms",
    );
}

MongoRunner.stopMongod(conn);
