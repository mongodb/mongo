//
// Ensures that the server respects the batch write command WriteConcern, and behaves correctly
//

var coll = db.getCollection("batch_write_wc");
coll.drop();

assert(coll.getDB().getMongo().useWriteCommands(), "test is not running with write commands");

// Basic validation of WriteConcern
// -- {}, versus {w:0}/{w:1} +opt wTimeout
// -- j:1, fsync:1,
// -- replication: w:N (>1), w:String, wTimeout
// -- randomField:true, etc
