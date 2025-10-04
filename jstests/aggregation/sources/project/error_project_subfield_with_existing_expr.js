// SERVER-6177: better error when projecting into a subfield with an existing expression

import {assertErrorCode} from "jstests/aggregation/extras/utils.js";

let c = db.c;
c.drop();

c.save({});

assertErrorCode(c, {$project: {"x": {$add: [1]}, "x.b": 1}}, 31249);
assertErrorCode(c, {$project: {"x.b": 1, "x": {$add: [1]}}}, 31250);
assertErrorCode(c, {$project: {"x": {"b": 1}, "x.b": 1}}, 31250);
assertErrorCode(c, {$project: {"x.b": 1, "x": {"b": 1}}}, 31250);
