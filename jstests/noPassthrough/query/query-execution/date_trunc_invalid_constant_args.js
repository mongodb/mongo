/**
 * Tests that $dateTrunc with invalid constant arguments reports a user error rather than tripping a
 * tripwire assertion when it is lowered into SBE. With pipeline optimization disabled the arguments
 * are not constant-folded, so the invalid constants reach SBE stage building.
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";

describe("$dateTrunc with invalid constant arguments lowered into SBE", function () {
    const date = new Date(0);

    before(function () {
        this.conn = MongoRunner.runMongod();
        assert.neq(this.conn, null, "mongod failed to start");

        this.testDB = this.conn.getDB(jsTestName());
        assert.commandWorked(this.testDB.coll.insert({_id: 0, date}));

        assert.commandWorked(
            this.testDB.adminCommand({
                setParameter: 1,
                internalQueryFrameworkControl: "trySbeRestricted",
            }),
        );
        assert.commandWorked(
            this.testDB.adminCommand({
                configureFailPoint: "disablePipelineOptimization",
                mode: "alwaysOn",
            }),
        );
    });

    after(function () {
        assert.commandWorked(
            this.testDB.adminCommand({
                configureFailPoint: "disablePipelineOptimization",
                mode: "off",
            }),
        );
        // A tripwire assertion makes mongod abort during shutdown, so a clean shutdown is what
        // asserts that the failures below were reported as user errors.
        MongoRunner.stopMongod(this.conn);
    });

    const testCases = [
        // 'timezone' that is not a string, and strings that are not valid timezones. Note the
        // embedded NUL byte, which is what the nul-bytes aggregation fuzzer generates.
        {spec: {unit: "minute", timezone: 1}, code: 7157928},
        {spec: {unit: "minute", timezone: "Not/A_Timezone"}, code: 7157929},
        {
            spec: {unit: "minute", timezone: "America/Regina" + String.fromCharCode(0) + "x"},
            code: 7157929,
        },
        // 'unit' that is not a string, and a string that is not a valid time unit.
        {spec: {unit: 1}, code: 7157933},
        {spec: {unit: "century"}, code: 7157934},
        // 'binSize' that is not a number, and numbers that are not positive 64-bit integers.
        {spec: {unit: "minute", binSize: "1"}, code: 7157937},
        {spec: {unit: "minute", binSize: NaN}, code: 7157938},
        {spec: {unit: "minute", binSize: 0}, code: 7157939},
        // 'startOfWeek' that is not a string, and a string that is not a valid day of the week.
        {spec: {unit: "week", startOfWeek: 1}, code: 7157941},
        {spec: {unit: "week", startOfWeek: "someday"}, code: 7157942},
    ];

    for (const {spec, code} of testCases) {
        it(`fails with ${code} for ${tojsononeline(spec)}`, function () {
            // Putting $dateTrunc in the group key forces it through SBE stage building.
            const dateTruncSpec = Object.assign({date}, spec);
            assert.commandFailedWithCode(
                this.testDB.runCommand({
                    aggregate: "coll",
                    pipeline: [{$group: {_id: {$dateTrunc: dateTruncSpec}}}],
                    cursor: {},
                }),
                code,
                "expected a user error",
                {dateTruncSpec},
            );
        });
    }
});
