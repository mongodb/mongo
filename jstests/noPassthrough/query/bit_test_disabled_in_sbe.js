/**
 * Tests that $bit* match expressions ($bitsAllSet, $bitsAllClear, $bitsAnySet, $bitsAnyClear) are
 * executed using the SBE engine when in trySbeEngine or featureFlagSbeFull mode, and use the
 * classic engine otherwise (forceClassicEngine or trySbeRestricted).
 */
import {getEngine} from "jstests/libs/query/analyze_plan.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";

const conn = MongoRunner.runMongod();
assert.neq(conn, null, "mongod failed to start up");
const db = conn.getDB(jsTestName());
const coll = db.bit_test_not_sbe;
coll.drop();

assert.commandWorked(coll.insert({x: 7}));

const expectSbe = checkSbeFullyEnabled(db);

for (const op of ["$bitsAllSet", "$bitsAllClear", "$bitsAnySet", "$bitsAnyClear"]) {
    const explain = coll.find({x: {[op]: [0, 1, 2]}}).explain();
    const expectedEngine = expectSbe ? "sbe" : "classic";
    assert.eq(
        getEngine(explain),
        expectedEngine,
        `expected ${op} to use ${expectedEngine} engine but got: ${tojson(explain)}`,
    );
}

MongoRunner.stopMongod(conn);
