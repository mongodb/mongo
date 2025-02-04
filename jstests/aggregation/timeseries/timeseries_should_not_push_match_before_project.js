// Regression test for SERVER-71270.
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";

const doc = {
    _id: 0,
    time: new Date('2019-01-18T13:24:15.443Z'),
    tag: {},
};

db.ts.drop();
db.coll.drop();

assert.commandWorked(
    db.createCollection('ts', {timeseries: {timeField: 'time', metaField: 'tag'}}));
assert.commandWorked(db.createCollection('coll'));

assert.commandWorked(db.ts.insertOne(doc));
assert.commandWorked(db.coll.insertOne(doc));
const pipeline = [
    {$project: {'time': 0}},
    {$match: {'time': {$lte: new Date('2019-02-13T11:36:03.481Z')}}},
];

const ts = db.ts.aggregate(pipeline).toArray();
const vanilla = db.coll.aggregate(pipeline).toArray();
assertArrayEq({
    actual: ts,
    expected: vanilla,
});