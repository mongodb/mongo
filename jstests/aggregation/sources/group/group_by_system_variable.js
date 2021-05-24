/*
 * Tests that the server doesn't crash when you group by a system variable.
 * Reproduces SERVER-57164.
 */
(function() {
"use strict";

const coll = db.group_by_system_variable;
coll.drop();
assert.commandWorked(coll.insert({}));

// These explains all should not throw.
coll.explain().aggregate({$group: {_id: "$$IS_MR"}});
coll.explain().aggregate({$group: {_id: "$$JS_SCOPE"}});
coll.explain().aggregate({$group: {_id: "$$CLUSTER_TIME"}});

// These queries throw, but the server should stay up.
assert.throws(() => coll.aggregate({$group: {_id: "$$IS_MR"}}));
assert.throws(() => coll.aggregate({$group: {_id: "$$JS_SCOPE"}}));
try {
    // This one may or may not throw: CLUSTER_TIME may or may not be defined,
    // depending on what kind of cluster we're running against.
    coll.aggregate({$group: {_id: "$$CLUSTER_TIME"}});
} catch (e) {
}
db.hello();
})();
