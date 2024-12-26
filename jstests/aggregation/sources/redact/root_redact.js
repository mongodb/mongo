// Usage of $$ROOT variable can lead to Document cache reallocation and possible memory corruption.
// Test is checking that this is not the case.
(function() {
"use strict";

const bigStr = 'X'.repeat(1024);
const doc = {
    _id: 0,
    d: true,
    sub: {d: false, str: bigStr},
    str: bigStr,
};

db.test.drop();
assert.commandWorked(db.test.insertOne(doc));

const pipeline = [{
    $redact: {
        $cond: {
            if: "$d",
            then: "$$DESCEND",
            else: {$cond: {if: {$objectToArray: "$$ROOT"}, then: "$$KEEP", else: "$$PRUNE"}}
        }
    }
}];

assert.eq([doc], db.test.aggregate(pipeline).toArray());
})();
