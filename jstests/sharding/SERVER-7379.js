var st = new ShardingTest({shards: 2});

st.adminCommand({enablesharding: "test"});
st.ensurePrimaryShard('test', 'shard0001');
st.adminCommand(
    {shardcollection: "test.offerChange", key: {"categoryId": 1, "store": 1, "_id": 1}});

var db = st.s.getDB('test');
var offerChange = db.getCollection('offerChange');
var testDoc = {"_id": 123, "categoryId": 9881, "store": "NEW"};

offerChange.remove({}, false);
offerChange.insert(testDoc);
assert.writeError(offerChange.update({_id: 123}, {$set: {store: "NEWEST"}}, true, false));
var doc = offerChange.findOne();
assert(friendlyEqual(doc, testDoc), 'doc changed: ' + tojson(doc));

offerChange.remove({}, false);
offerChange.insert(testDoc);
assert.writeError(
    offerChange.update({_id: 123}, {_id: 123, categoryId: 9881, store: "NEWEST"}, true, false));
doc = offerChange.findOne();
assert(friendlyEqual(doc, testDoc), 'doc changed: ' + tojson(doc));

offerChange.remove({}, false);
offerChange.insert(testDoc);
assert.writeError(offerChange.save({"_id": 123, "categoryId": 9881, "store": "NEWEST"}));
doc = offerChange.findOne();
assert(friendlyEqual(doc, testDoc), 'doc changed: ' + tojson(doc));

offerChange.remove({}, false);
offerChange.insert(testDoc);
assert.writeError(offerChange.update(
    {_id: 123, store: "NEW"}, {_id: 123, categoryId: 9881, store: "NEWEST"}, true, false));
doc = offerChange.findOne();
assert(friendlyEqual(doc, testDoc), 'doc changed: ' + tojson(doc));

offerChange.remove({}, false);
offerChange.insert(testDoc);
assert.writeError(offerChange.update(
    {_id: 123, categoryId: 9881}, {_id: 123, categoryId: 9881, store: "NEWEST"}, true, false));
doc = offerChange.findOne();
assert(friendlyEqual(doc, testDoc), 'doc changed: ' + tojson(doc));

st.stop();
