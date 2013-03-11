var coll = "metricstest";
var t = db.getCollection( coll, {capped:true, size:1000} );

var initMetr = db.serverStatus().metrics.document
t.drop();
db.createCollection( coll );

//Test insert and delete
t.insert({_id:1, x:1})
t.remove({x:1})
var newMetr = db.serverStatus().metrics.document
assert.gt(newMetr.deleted, initMetr.deleted, 
	"Document deleted");
assert.gt(newMetr.inserted, initMetr.inserted, 
	"Document inserted");

//Test number returned
t.insert({_id:1,x:1})
t.insert({_id:2,x:2})
t.insert({_id:3,x:3})
t.insert({_id:4,x:1})
t.insert({_id:5,x:2})
t.insert({_id:6,x:3})
var newMetr = db.serverStatus().metrics.document
var res = t.findOne()
var newMetr = db.serverStatus().metrics.document
assert.gt(newMetr.returned, initMetr.returned, 
	"Document returned");
//Reset initial for next round
initMetr = newMetr

//Test Update and scanned
t.update({x:3}, {x:4})
var newMetr = db.serverStatus().metrics.document
assert.gt(newMetr.scanned, initMetr.scanned, 
	"Documents scanned");
assert.gt(newMetr.updated, initMetr.updated, 
	"Document updated");
t.update({x:3}, {x:5})
var newMetr = db.serverStatus().metrics.document
assert.gt(newMetr.scanned, initMetr.scanned, 
	"Documents scanned");
assert.gt(newMetr.updated, initMetr.updated, 
	"Documents updated");

//Test idhack
var initMetr = db.serverStatus().metrics.operation
t.find({_id:4}).itcount()
var newMetr = db.serverStatus().metrics.operation
assert.gt(newMetr.idhack + 1, initMetr.idhack, 
	"Test idhack increased");

//Test scanAndOrder
var initMetr = db.serverStatus().metrics.operation
t.find().sort( {x:1} ).itcount();
var newMetr = db.serverStatus().metrics.operation
assert.gt(newMetr.scanAndOrder + 1, initMetr.scanAndOrder, 
	"Test scanAndOrder increased");

//Test Fastmod
t.update({x:1}, {$inc: {x:1}}, false, false)
var newMetr = db.serverStatus().metrics.operation
assert.gt(newMetr.fastmod + 1, initMetr.fastmod, 
	"Test fastmod increased");
