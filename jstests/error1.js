db.jstests_error1.drop();

// test 1
db.$cmd.findOne({reseterror:1});
assert( db.$cmd.findOne({getlasterror:1}).err == null, "A" );
assert( db.$cmd.findOne({getpreverror:1}).err == null, "B" );

db.resetError();
assert( db.getLastError() == null, "C" );
assert( db.getPrevError().err == null , "preverror 1" );

// test 2

db.$cmd.findOne({forceerror:1});
assert( db.$cmd.findOne({getlasterror:1}).err != null, "D" );
assert( db.$cmd.findOne({getpreverror:1}).err != null, "E" );


assert( db.getLastError() != null, "F" );
assert( db.getPrevError().err != null , "preverror 2" );
assert( db.getPrevError().nPrev == 1, "G" );

db.jstests_error1.findOne();
assert( db.$cmd.findOne({getlasterror:1}).err == null, "H" );
assert( db.$cmd.findOne({getpreverror:1}).err != null, "I" );
assert( db.$cmd.findOne({getpreverror:1}).nPrev == 2, "J" );

db.jstests_error1.findOne();
assert( db.$cmd.findOne({getlasterror:1}).err == null, "K" );
assert( db.$cmd.findOne({getpreverror:1}).err != null, "L" );
assert( db.$cmd.findOne({getpreverror:1}).nPrev == 3, "M" );

db.resetError();
db.forceError();
db.jstests_error1.findOne();
assert( db.getLastError() == null , "getLastError 5" );
assert( db.getPrevError().err != null , "preverror 3" );

// test 3
db.$cmd.findOne({reseterror:1});
assert( db.$cmd.findOne({getpreverror:1}).err == null, "N" );
