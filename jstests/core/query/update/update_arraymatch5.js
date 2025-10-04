// @tags: [
//   requires_fastcount,
//   requires_multi_updates,
//   requires_non_retryable_writes,
// ]

const t = db[jsTestName()];
t.drop();

assert.commandWorked(t.insert({abc: {visible: true}, testarray: [{foobar_id: 316, visible: true, xxx: 1}]}));
assert.commandWorked(t.createIndex({"abc.visible": 1, "testarray.visible": 1, "testarray.xxx": 1}));
assert(t.findOne({"abc.visible": true, testarray: {"$elemMatch": {visible: true, xxx: 1}}}), "A1");
assert(t.findOne({testarray: {"$elemMatch": {visible: true, xxx: 1}}}), "A2");

assert.commandWorked(
    t.update({"testarray.foobar_id": 316}, {"$set": {"testarray.$.visible": true, "testarray.$.xxx": 2}}, false, true),
);

assert(t.findOne(), "B1");
assert(t.findOne({testarray: {"$elemMatch": {visible: true, xxx: 2}}}), "B2");
assert(t.findOne({"abc.visible": true, testarray: {"$elemMatch": {visible: true, xxx: 2}}}), "B3");
assert.eq(1, t.find().count(), "B4");
