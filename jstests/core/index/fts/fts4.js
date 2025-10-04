import {queryIDS} from "jstests/libs/fts.js";

let t = db.text4;
t.drop();

t.save({_id: 1, x: ["az", "b", "x"], y: ["c", "d", "m"], z: 1});
t.save({_id: 2, x: ["c", "d", "y"], y: ["az", "b", "n"], z: 2});

t.createIndex({y: "text", z: 1}, {weights: {x: 10}});

assert.eq([1, 2], queryIDS(t, "az"), "A1");
assert.eq([2, 1], queryIDS(t, "d"), "A2");

assert.eq([1], queryIDS(t, "x"), "A3");
assert.eq([2], queryIDS(t, "y"), "A4");

assert.eq([1], queryIDS(t, "az", {z: 1}), "B1");
assert.eq([1], queryIDS(t, "d", {z: 1}), "B2");
