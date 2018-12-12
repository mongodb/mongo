db.jstests_error1.drop();

// test 1
db.runCommand({reseterror: 1});
assert(db.runCommand({getlasterror: 1}).err == null, "A");

db.resetError();
assert(db.getLastError() == null, "C");

// test 2

db.runCommand({insert: "x", documents: [{_id: 1, a: "xx"}, {_id: 1, b: "xx"}]});
assert(db.runCommand({getlasterror: 1}).err != null, "D");

assert(db.getLastError() != null, "F");

db.jstests_error1.findOne();
assert(db.runCommand({getlasterror: 1}).err == null, "H");

db.jstests_error1.findOne();
assert(db.runCommand({getlasterror: 1}).err == null, "K");

db.resetError();
db.forceError();
db.jstests_error1.findOne();
assert(db.getLastError() == null, "getLastError 5");

// test 3
db.runCommand({reseterror: 1});
