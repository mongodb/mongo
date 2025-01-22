// server-6198: disallow dots in group output field names
import {assertErrorCode} from "jstests/aggregation/extras/utils.js";

const coll = db[jsTestName()];
coll.drop();

assertErrorCode(coll, {$group: {_id: null, "bar.baz": {$addToSet: "$foo"}}}, 40235);
