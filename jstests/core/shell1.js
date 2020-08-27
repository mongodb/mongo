x = 1;

shellHelper("show", "tables;");
shellHelper("show", "tables");
shellHelper("show", "tables ;");

// test secondaryOk levels
assert(!db.getSecondaryOk() && !db.test.getSecondaryOk() && !db.getMongo().getSecondaryOk(),
       "secondaryOk 1");
db.getMongo().setSecondaryOk();
assert(db.getSecondaryOk() && db.test.getSecondaryOk() && db.getMongo().getSecondaryOk(),
       "secondaryOk 2");
db.setSecondaryOk(false);
assert(!db.getSecondaryOk() && !db.test.getSecondaryOk() && db.getMongo().getSecondaryOk(),
       "secondaryOk 3");
db.test.setSecondaryOk();
assert(!db.getSecondaryOk() && db.test.getSecondaryOk() && db.getMongo().getSecondaryOk(),
       "secondaryOk 4");
