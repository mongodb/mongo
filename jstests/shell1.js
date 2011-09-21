
x = 1;

shellHelper( "show", "tables;" )
shellHelper( "show", "tables" )
shellHelper( "show", "tables ;" )

// test verbose shell option
setVerboseShell();
var res = db.test.remove({a:1});
var res2 = db.test.update({a:1}, {b: 1});
assert(res != undefined && res2 != undefined, "verbose shell 1")
setVerboseShell(false);
var res = db.test.remove({a:1});
assert(res == undefined, "verbose shell 2")

// test slaveOk levels
assert(!db.getSlaveOk() && !db.test.getSlaveOk() && !db.getMongo().getSlaveOk(), "slaveOk 1");
db.getMongo().setSlaveOk();
assert(db.getSlaveOk() && db.test.getSlaveOk() && db.getMongo().getSlaveOk(), "slaveOk 2");
db.setSlaveOk(false);
assert(!db.getSlaveOk() && !db.test.getSlaveOk() && db.getMongo().getSlaveOk(), "slaveOk 3");
db.test.setSlaveOk(true);
assert(!db.getSlaveOk() && db.test.getSlaveOk() && db.getMongo().getSlaveOk(), "slaveOk 4");

