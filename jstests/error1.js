
// test 1
db.$cmd.findOne({reseterror:1});
assert( db.$cmd.findOne({getlasterror:1}).err == null );
assert( db.$cmd.findOne({getpreverror:1}).err == null );

db.resetError();
assert( db.getLastError() == null );
assert( db.getPrevError().err == null , "preverror 1" );

// test 2

db.$cmd.findOne({forceerror:1});
assert( db.$cmd.findOne({getlasterror:1}).err != null );
assert( db.$cmd.findOne({getpreverror:1}).err != null );


assert( db.getLastError() != null );
assert( db.getPrevError().err != null , "preverror 2" );
assert( db.getPrevError().nPrev == 1 );

db.foo.findOne();
assert( db.$cmd.findOne({getlasterror:1}).err == null );
assert( db.$cmd.findOne({getpreverror:1}).err != null );
assert( db.$cmd.findOne({getpreverror:1}).nPrev == 2 );

db.foo.findOne();
assert( db.$cmd.findOne({getlasterror:1}).err == null );
assert( db.$cmd.findOne({getpreverror:1}).err != null );
assert( db.$cmd.findOne({getpreverror:1}).nPrev == 3 );

db.resetError();
db.forceError();
db.foo.findOne();
assert( db.getLastError() == null , "getLastError 5" );
assert( db.getPrevError().err != null , "preverror 3" );

// test 3
db.$cmd.findOne({reseterror:1});
assert( db.$cmd.findOne({getpreverror:1}).err == null );
