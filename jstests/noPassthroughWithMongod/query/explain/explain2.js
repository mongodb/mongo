// Test for race condition SERVER-2807.  One cursor is dropped and another is not.
// @tags: [requires_capped]

import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";

let collName = 'jstests_slowNightly_explain2';

let t = db[collName];
t.drop();

db.createCollection(collName, {capped: true, size: 100000});
t = db[collName];
t.createIndex({x: 1});

let a = startParallelShell(funWithArgs(function(collName) {
                               for (let i = 0; i < 50000; ++i) {
                                   db[collName].insert({x: i, y: 1});
                               }
                           }, collName), db.getMongo().port);

for (let i = 0; i < 800; ++i) {
    t.find({x: {$gt: -1}, y: 1}).sort({x: -1}).explain();
}

a();
