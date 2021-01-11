(function() {
const emrcFalseConn =
    MongoRunner.runMongod({storageEngine: "devnull", enableMajorityReadConcern: false});
assert(!emrcFalseConn);
var logContents = rawMongoProgramOutput();
assert(logContents.indexOf("enableMajorityReadConcern:false is no longer supported") > 0);

// Even though enableMajorityReadConcern: true is the default, the server internally changes
// this value to false when running with the devnull storage engine.
const emrcDefaultConn = MongoRunner.runMongod({storageEngine: "devnull"});
db = emrcDefaultConn.getDB("test");

res = db.foo.insert({x: 1});
assert.eq(1, res.nInserted, tojson(res));

// Skip collection validation during stopMongod if invalid storage engine.
TestData.skipCollectionAndIndexValidation = true;

MongoRunner.stopMongod(emrcDefaultConn);
}());
