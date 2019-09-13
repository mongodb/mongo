/*
 * Test that dropIndexes does not return a message containing multiple "nIndexesWas" fields.
 *
 * @tags: [requires_wiredtiger]
 */

(function() {
"use strict";

let oldRes, newRes;
let coll = db.dropindexes_duplicate_fields;

try {
    // Repeat 100 times for the sake of probabilities
    for (let i = 0; i < 100; i++) {
        coll.drop();
        assert.commandWorked(coll.insert({x: 1}));
        assert.commandWorked(coll.createIndex({"x": 1}));

        assert.commandWorked(db.adminCommand(
            {configureFailPoint: 'WTWriteConflictException', mode: {activationProbability: 0.1}}));

        // Will blow up if writeConflictRetry causes duplicate fields to be appended to result
        let res = db.runCommand({dropIndexes: coll.getName(), index: {"x": 1}});
        if (!oldRes && !newRes) {
            oldRes = res;
        } else if (oldRes && !newRes) {
            newRes = res;
        } else {
            oldRes = newRes;
            newRes = res;
        }

        assert.commandWorked(newRes ? newRes : oldRes);

        // Responses between runs should be the same independent of a WriteConflict
        if (oldRes && newRes) {
            assert.eq(oldRes, newRes);
        }

        assert.commandWorked(
            db.adminCommand({configureFailPoint: 'WTWriteConflictException', mode: "off"}));
    }
} finally {
    assert.commandWorked(
        db.adminCommand({configureFailPoint: 'WTWriteConflictException', mode: "off"}));
}
})();
