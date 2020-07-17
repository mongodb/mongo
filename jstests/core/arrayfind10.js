/**
 * Tests some of the find command's quirky semantics regarding how arrays are handled in various
 * situations. Includes tests for bugs described in SERVER-49720.
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // arrayEq

const t = db.jstests_arrayfind10;
t.drop();

t.save({a: "foo"});
t.save({a: ["foo"]});
t.save({a: [["foo"]]});
t.save({a: [[["foo"]]]});

function doTest1(t) {
    assert(arrayEq(t.find({a: "foo"}, {_id: 0}).toArray(), [{a: "foo"}, {a: ["foo"]}]));
}

doTest1(t);
t.ensureIndex({'a': 1});
doTest1(t);

t.drop();
})();
