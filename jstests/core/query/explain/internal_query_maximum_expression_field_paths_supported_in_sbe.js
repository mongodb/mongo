/**
 * Tests that SBE is disabled when the number of ExpressionFieldPath path
 * components exceeds internalQueryMaxNumExprFieldPathComponentsSupportedInSbe.
 *
 * Verifies user defined variables' path components count once at declaration,
 * regardless of how many times the variable is referenced in the query.
 *
 * @tags: [
 *  # For simplicity of explain analysis, this test does not run against sharded collections.
 *  assumes_against_mongod_not_mongos,
 *  assumes_standalone_mongod,
 *  assumes_unsharded_collection,
 *  # Refusing to run a test that issues commands that may return different values after a failover
 *  does_not_support_stepdowns,
 *  # Explain for the aggregate command cannot run within a multi-document transaction
 *  does_not_support_transactions,
 *  requires_fcv_83,
 *  # This test sets a server parameter via setParameterOnAllNonConfigNodes. To keep the host list
 *  # consistent, no add/remove shard operations should occur during the test.
 *  assumes_stable_shard_list,
 * ]
 */
import {getEngine} from "jstests/libs/query/analyze_plan.js";
import {setParameterOnAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

function checkEngine(stage, threshold, max, seenClassic, seenSbe) {
    const engine = getEngine(db.c.explain().aggregate(stage));
    if (threshold > max) {
        assert(engine === "classic", "expected engine classic");
        return {seenClassic: true, seenSbe};
    } else {
        assert(engine === "sbe", "expected engine SBE");
        return {seenClassic, seenSbe: true};
    }
}

const origMaxExpr = db.adminCommand({getParameter: 1, "internalQueryMaxNumExprFieldPathComponentsSupportedInSbe": 1});
const origFramework = db.adminCommand({getParameter: 1, "internalQueryFrameworkControl": 1});
db.c.drop();
db.c.insert({_id: 1, a: 1});

const testCases = [
    {
        buildStage: (paths) => ({$group: {_id: paths}}),
        threshold: (n) => 2 * n,
        flags: {seenClassic: false, seenSbe: false},
        msg: "no UDV",
    },
    {
        buildStage: (paths, refs) => ({$group: {_id: {$let: {vars: paths, in: refs}}}}),
        threshold: (n) => 4 * n,
        flags: {seenClassic: false, seenSbe: false},
        msg: "UDV",
    },
    {
        // $let path components count once at declaration, regardless of reference count.
        buildStage: (paths, refs) => ({$group: {_id: {$let: {vars: paths, in: refs[0]}}}}),
        threshold: (n) => 2 * n + 1,
        flags: {seenClassic: false, seenSbe: false},
        msg: "UDV one reference",
    },
];

try {
    setParameterOnAllNonConfigNodes(db.getMongo(), "internalQueryFrameworkControl", "trySbeEngine");
    for (let max = 4; max <= 256; max *= 2) {
        setParameterOnAllNonConfigNodes(db.getMongo(), "internalQueryMaxNumExprFieldPathComponentsSupportedInSbe", max);
        for (let n of [max / 4 - 1, max / 4, max / 4 + 1, max / 2 - 1, max / 2, max / 2 + 1]) {
            const paths = {};
            const refs = [];
            for (let i = 0; i < n; i++) {
                const key = `ab${i}`;
                paths[key] = `$a.b${i}`;
                refs.push(`$$${key}.c`);
            }
            for (const tc of testCases) {
                const result = checkEngine(
                    tc.buildStage(paths, refs),
                    tc.threshold(n),
                    max,
                    tc.flags.seenClassic,
                    tc.flags.seenSbe,
                );
                tc.flags.seenClassic = result.seenClassic;
                tc.flags.seenSbe = result.seenSbe;
            }
        }
        for (const tc of testCases) {
            assert(tc.flags.seenClassic, `expected seen classic, ${tc.msg}`);
            assert(tc.flags.seenSbe, `expected seen SBE, ${tc.msg}`);
        }
    }
} finally {
    setParameterOnAllNonConfigNodes(
        db.getMongo(),
        "internalQueryMaxNumExprFieldPathComponentsSupportedInSbe",
        origMaxExpr.internalQueryMaxNumExprFieldPathComponentsSupportedInSbe,
    );
    setParameterOnAllNonConfigNodes(
        db.getMongo(),
        "internalQueryFrameworkControl",
        origFramework.internalQueryFrameworkControl,
    );
}
