/*
 * SERVER-15033 truncate on a regular collection
 *
 * The test runs commands that are not allowed with security token: emptycapped.
 * @tags: [
 *   not_allowed_with_security_token,
 *   requires_collstats,
 *   requires_non_retryable_commands,
 *   uses_testing_only_commands,
 *   requires_emptycapped,
 *   uses_full_validation,
 *   no_selinux,
 *   # TODO SERVER-60823 re-add transaction support
 *   does_not_support_transactions,
 * ]
 */

(function() {
'use strict';

var t = db.getCollection('collection_truncate');
t.drop();

function truncate() {
    // Until SERVER-15274 is implemented, this is the only way to truncate a collection.
    assert.commandWorked(t.runCommand('emptycapped'));  // works on non-capped as well.
}

function assertEmpty() {
    var stats = t.stats();

    assert.eq(stats.count, 0);
    assert.eq(stats.size, 0);

    assert.eq(t.count(), 0);
    assert.eq(t.find().itcount(), 0);

    var res = t.validate({full: true});
    assert.commandWorked(res);
    assert(res.valid, "failed validate(): " + tojson(res));
}

// Single record case.
t.insert({a: 1});
truncate();
assertEmpty();

// Multi-extent case.
const initialStorageSize = t.stats().storageSize;
const long_string = Array(1024 * 1024).toString();

let idx = 0;
while (t.stats().storageSize == initialStorageSize) {
    let bulk = t.initializeUnorderedBulkOp();
    const nDocs = 300;
    for (let i = 0; i < nDocs; i++) {
        bulk.insert({a: ++idx, text: long_string});
    }
    assert.commandWorked(bulk.execute());
}
jsTest.log("Initial storage size: " + initialStorageSize);
jsTest.log("Num inserts: " + idx);
jsTest.log("Storage size after inserts: " + t.stats().storageSize);

truncate();
assertEmpty();

// Already empty case.
truncate();
assertEmpty();
}());
