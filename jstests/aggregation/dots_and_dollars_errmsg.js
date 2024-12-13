/**
 * Tests that the dots and dollars hint encouraging users to use $getField/$setField is only
 * provided when that is a valid option.
 *
 * @tags: [ featureFlagRankFusionFull, requires_fcv_81 ]
 */

import {
    assertErrMsgContains,
    assertErrMsgDoesNotContain
} from "jstests/aggregation/extras/utils.js";

const collName = jsTestName();
const coll = db[collName];
coll.drop();

const dotsAndDollarsMsg = "$getField or $setField";

let pipeline = [{$project: {"$a": 1}}];
assertErrMsgContains(coll, pipeline, dotsAndDollarsMsg);

pipeline = [{$addFields: {"a": 1, "$otherField": 1}}];
assertErrMsgContains(coll, pipeline, dotsAndDollarsMsg);

pipeline = [{$facet: {"$foo": []}}];
assertErrMsgDoesNotContain(coll, pipeline, dotsAndDollarsMsg);
assertErrMsgContains(coll, pipeline, "$facet pipeline names");

pipeline = [{$rankFusion: {input: {pipelines: {"$xyz": [{$sort: {a: 1}}]}}}}];
assertErrMsgDoesNotContain(coll, pipeline, dotsAndDollarsMsg);
assertErrMsgContains(coll, pipeline, "$rankFusion pipeline names");
