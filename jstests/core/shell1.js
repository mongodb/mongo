x = 1;

shellHelper( "show", "tables;" )
shellHelper( "show", "tables" )
shellHelper( "show", "tables ;" )

// test slaveOk levels
assert(!db.getSlaveOk() && !db.test.getSlaveOk() && !db.getMongo().getSlaveOk(), "slaveOk 1");
db.getMongo().setSlaveOk();
assert(db.getSlaveOk() && db.test.getSlaveOk() && db.getMongo().getSlaveOk(), "slaveOk 2");
db.setSlaveOk(false);
assert(!db.getSlaveOk() && !db.test.getSlaveOk() && db.getMongo().getSlaveOk(), "slaveOk 3");
db.test.setSlaveOk(true);
assert(!db.getSlaveOk() && db.test.getSlaveOk() && db.getMongo().getSlaveOk(), "slaveOk 4");

