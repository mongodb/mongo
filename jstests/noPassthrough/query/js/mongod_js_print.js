/**
 * Server-side JS print() and error output is truncated, and print() is capped per scope, so user JS
 * cannot flood the log.
 *
 * @tags: [
 *   requires_scripting,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";

const kBudgetBytes = 64 * 1024;
const kLineBytes = 1024;
const kMaxAttrBytes = 10 * 1024;

// rawMongoProgramOutput() already keeps only the lines matching 'needle'.
function outputLines(needle) {
    return rawMongoProgramOutput(needle).split("\n").filter(Boolean);
}

describe("server-side print()", function () {
    before(function () {
        this.conn = MongoRunner.runMongod();
        this.coll = this.conn.getDB("test")[jsTestName()];
        assert.commandWorked(this.coll.insert({_id: 0}));

        const aggregateFunction = (body, args) =>
            this.coll.aggregate([{$project: {r: {$function: {body, args, lang: "js"}}}}]).toArray();

        const waitForOutput = () => {
            aggregateFunction(
                function (marker) {
                    print(marker.split("").reverse().join(""));
                },
                ["ENODTUPTUO"],
            );
            assert.soon(() => outputLines("OUTPUTDONE").length > 0);
        };

        // Runs 'body' server-side, then waits until all of its output can be read.
        this.runFunction = (body, args) => {
            clearRawMongoProgramOutput();
            try {
                return aggregateFunction(body, args);
            } finally {
                waitForOutput();
            }
        };

        // The marker is reversed inside the function so the expected text never appears verbatim
        // in command or slow-query logs.
        this.runPrints = (marker, numPrints, lineBytes) =>
            this.runFunction(
                function (marker, numPrints, lineBytes) {
                    const line = marker.split("").reverse().join("").padEnd(lineBytes, "x");
                    for (let i = 0; i < numPrints; i++) {
                        print(line);
                    }
                    return numPrints;
                },
                [marker, numPrints, lineBytes],
            );
    });

    after(function () {
        MongoRunner.stopMongod(this.conn);
    });

    it("stops at the budget and logs a single suppression notice", function () {
        this.runPrints("ATTOR", 2 * (kBudgetBytes / kLineBytes), kLineBytes);
        assert.eq(outputLines("Suppressing further JavaScript print").length, 1);
        assert.eq(outputLines("ROTTA").length, kBudgetBytes / kLineBytes);
    });

    it("gives the next operation a fresh budget", function () {
        this.runPrints("HSERF", 1, kLineBytes);
        assert.eq(outputLines("FRESH").length, 1);
    });

    it("drops prints whose stringification exhausted the budget", function () {
        const numInner = 2 * (kBudgetBytes / kLineBytes);
        const results = this.runFunction(
            function (innerMarker, outerMarker, numInner, lineBytes) {
                const line = innerMarker.split("").reverse().join("").padEnd(lineBytes, "x");
                let lateCoerced = false;
                const late = {
                    toString: function () {
                        lateCoerced = true;
                        return "late";
                    },
                };
                const hungry = {
                    toString: function () {
                        for (let i = 0; i < numInner; i++) {
                            print(line);
                        }
                        return outerMarker.split("").reverse().join("");
                    },
                };
                print(hungry, late);
                return lateCoerced;
            },
            ["RENNI", "RETUO", numInner, kLineBytes],
        );
        assert.eq(outputLines("INNER").length, kBudgetBytes / kLineBytes);
        assert.eq(outputLines("Suppressing further JavaScript print").length, 1);
        // The re-entrant prints exhausted the budget, so the outer print must not log on unwind...
        const outerLines = outputLines("OUTER");
        assert.eq(outerLines.length, 0, {output: outerLines});
        // ...and the in-loop recheck must stop before coercing the remaining argument.
        assert.eq(results[0].r, false);
    });

    it("truncates an oversized error stack", function () {
        assert.throws(() =>
            this.runFunction(
                function (marker, stackBytes) {
                    const e = new Error("x");
                    e.stack = marker.split("").reverse().join("").padEnd(stackBytes, "x");
                    throw e;
                },
                ["KCATS", 4 * kMaxAttrBytes],
            ),
        );
        // On mongod the stack always reaches the log via the JS error log line; the slow query
        // log's errMsg may add further lines. All of them must be bounded.
        const stackLines = outputLines("STACK");
        assert.gt(stackLines.length, 0, {output: stackLines});
        for (const line of stackLines) {
            assert.lt(line.length, 2 * kMaxAttrBytes, "error stack was not truncated", {
                length: line.length,
            });
        }
    });

    it("truncates a single oversized print", function () {
        this.runPrints("GIBEL", 1, 4 * kMaxAttrBytes);
        const lines = outputLines("LEBIG");
        assert.eq(lines.length, 1);
        assert.lt(lines[0].length, 2 * kMaxAttrBytes, "print() output was not truncated", {
            length: lines[0].length,
        });
    });
});
