// @tags: [
//   requires_non_retryable_commands,
//   requires_non_retryable_writes,
//   uses_multiple_connections,
//   uses_parallel_shell,
// ]

// Test unsafe management of nsdt on update command yield SERVER-3208

let prefixNS = db.jstests_updatef;
assert.commandWorked(prefixNS.save({}));

let t = db.jstests_updatef_actual;
t.drop();

assert.commandWorked(t.save({a: 0, b: []}));
for (let i = 0; i < 1000; ++i) {
    assert.commandWorked(t.save({a: 100}));
}
assert.commandWorked(t.save({a: 0, b: []}));

// Repeatedly rename jstests_updatef to jstests_updatef_ and back.  This will
// invalidate the jstests_updatef_actual NamespaceDetailsTransient object.
let s = startParallelShell(function() {
    for (let i = 0; i < 100; ++i) {
        db.jstests_updatef.renameCollection('jstests_updatef_');
        db.jstests_updatef_.renameCollection('jstests_updatef');
    }
});

for (let i = 0; i < 20; ++i) {
    assert.commandWorked(t.update({a: 0}, {$push: {b: i}}, false, true));
}

s();
