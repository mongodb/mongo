/**
 * Tests that $bit* match expressions ($bitsAllSet, $bitsAllClear, $bitsAnySet, $bitsAnyClear) are
 * executed using the SBE engine only when SBE is fully enabled (trySbeEngine or featureFlagSbeFull),
 * and use the classic engine otherwise. As a memory-usage mitigation these operators are marked
 * 'requiresTrySbe', so they never trigger SBE on their own but can still be planned in SBE when it
 * is fully enabled.
 */
import {getEngine} from "jstests/libs/query/analyze_plan.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";

const kBitTestOperators = ["$bitsAllSet", "$bitsAllClear", "$bitsAnySet", "$bitsAnyClear"];

const coll = db.bit_test_limited_sbe;
coll.drop();

assert.commandWorked(coll.insert({x: 7}));

const expectSbe = checkSbeFullyEnabled(db);

for (const op of kBitTestOperators) {
    const explain = coll.find({x: {[op]: [0, 1, 2]}}).explain();
    const expectedEngine = expectSbe ? "sbe" : "classic";
    assert.eq(
        getEngine(explain),
        expectedEngine,
        `expected ${op} to use ${expectedEngine} engine but got: ${tojson(explain)}`,
    );
}

// Run a query which contains an expression completely unsupported in SBE followed by a $bits*
// expression which only runs in 'trySbeEngine'. The result should be that the query always uses
// classic.
for (const op of kBitTestOperators) {
    const pipe = [
        {
            $match: {
                "a": {"$_internalSchemaType": ["number"]},
                x: {[op]: [0, 1, 2]},
            },
        },
        {$group: {_id: 1, sum: {$sum: 1}}},
    ];

    const explain = coll.explain().aggregate(pipe);
    assert.eq(
        getEngine(explain),
        "classic",
        `expected ${op} to use classic engine but got: ${tojson(explain)}`,
    );
}

coll.drop();
