/**
 * This test validates a reader concurrent with a multikey update observes a proper snapshot of
 * data. Specifically, if the data can be multikey, the in-memory `isMultikey()` method must return
 * true. The `validate` command ensures this relationship.
 *
 * The failpoint used just widens a window, instead of pausing execution. Thus the test is
 * technically non-deterministic. However, if the server had a bug that caused a regression, the
 * non-determinism would cause the test to sometimes pass when it should fail. A properly behaving
 * server should never cause the test to accidentally fail.
 *
 * @tags: [requires_persistence]
 */
(function() {
'use strict';

db.foo.drop();

assert.commandWorked(db.foo.createIndex({a: 1}));
let validation = assert.commandWorked(db.foo.validate({background: true}));
jsTestLog({validation: validation});
let join = startParallelShell(function() {
    assert.commandWorked(
        db.adminCommand({configureFailPoint: "widenWUOWChangesWindow", mode: "alwaysOn"}));
    // This first insert flips multikey.
    assert.commandWorked(db.foo.insert({a: [1, 2]}));
    // This second insert is just a signal to allow the following validation loop to gracefully
    // break.
    assert.commandWorked(db.foo.insert({}));
}, db.getMongo().port);

while (db.foo.count() < 2) {
    validation = assert.commandWorked(db.foo.validate({background: true}));
    jsTestLog({background: validation});
    assert(validation.valid);
}

assert.commandWorked(db.adminCommand({configureFailPoint: "widenWUOWChangesWindow", mode: "off"}));
join();
})();
