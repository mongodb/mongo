// $_internalConstructNdvSketch is internal-only: only the analyze command may run it, so user
// requests must be rejected at parse time. The happy path is covered end to end by the analyze
// mode: "ndv" tests (SERVER-131758).
// @tags: [
//     # The accumulator does not exist on older binaries, which fail with "unknown group
//     # operator" instead of the internal-only rejection.
//     requires_fcv_90,
// ]
import {before, describe, it} from "jstests/libs/mochalite.js";

describe("$_internalConstructNdvSketch", function () {
    before(function () {
        this.coll = db[jsTestName()];
        this.coll.drop();
        assert.commandWorked(this.coll.insert({a: 1}));
    });

    it("is rejected in user requests", function () {
        assert.commandFailedWithCode(
            db.runCommand({
                aggregate: this.coll.getName(),
                pipeline: [
                    {
                        $group: {
                            _id: null,
                            sketches: {
                                $_internalConstructNdvSketch: {val: "$$ROOT", fields: ["a"]},
                            },
                        },
                    },
                ],
                cursor: {},
            }),
            5491300,
        );
    });
});
