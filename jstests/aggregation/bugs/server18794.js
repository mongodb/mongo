
var t = db.server18794;
t.drop();

assert.writeOK(t.insert({"_id" : 0, "data" : {"a" : "1", "b" : "2", "c" : "3"}}));
assert.writeOK(t.insert({"_id" : 1, "data" : {"d" : "4",}}));
assert.writeOK(t.insert({"_id" : 2, "data" : {}}));

assert.writeOK(t.insert({"_id" : 3, "data" : NaN}));
assert.writeOK(t.insert({"_id" : 4, "data" : null}));
assert.writeOK(t.insert({"_id" : 5, "data" : undefined}));

assert.writeOK(t.insert({"_id" : 6}));

var results = t.aggregate([{$objectToArray : "$data"}]).toArray();

var expectedResults = [
	{"_id" : 0, "data" : [{"a" : "1"}, {"b" : "2"}, {"c" : "3"}]},
	{"_id" : 1, "data" : [{"d" : "4"}]},
	{"_id" : 2, "data" : []},
	{"_id" : 3, "data" : []},
	{"_id" : 4, "data" : []},
	{"_id" : 5, "data" : []},
	{"_id" : 6, "data" : []}
];

assert.eq(results, expectedResults);