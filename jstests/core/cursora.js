// @tags: [
//   requires_fastcount,
//   requires_getmore,
//   requires_non_retryable_writes,
//   # Uses $where operator
//   requires_scripting,
//   uses_multiple_connections,
// ]

(function() {
"use strict";

const t = db.cursora;

function run(n) {
    if (!isNumber(n)) {
        assert(isNumber(n), "cursora.js isNumber");
    }
    t.drop();

    let bulk = t.initializeUnorderedBulkOp();
    for (let i = 0; i < n; i++)
        bulk.insert({_id: i});
    assert.commandWorked(bulk.execute());

    const join = startParallelShell("sleep(50);" +
                                    "db.cursora.remove({});");

    let num;
    try {
        let start = new Date();
        num = t.find(function() {
                   let num = 2;
                   for (let x = 0; x < 1000; x++)
                       num += 2;
                   return num > 0;
               })
                  .sort({_id: -1})
                  .itcount();
    } catch (e) {
        print("cursora.js FAIL " + e);
        join();
        throw e;
    }

    join();

    assert.eq(0, t.count());
    if (n == num)
        print("cursora.js warning: shouldn't have counted all  n: " + n + " num: " + num);
}

run(1500);
run(5000);
})();
