/**
 * Validates that empty fields are banned in $group.
 * @tags: [
 *   requires_fcv_90,
 * ]
 */
import {assertErrorCode} from "jstests/aggregation/extras/utils.js";

const coll = db[jsTestName()];
coll.drop();
coll.insertOne({});

assertErrorCode(coll, {$group: {_id: null, "": {$avg: "$a"}}}, 12116300);
