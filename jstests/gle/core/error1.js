db.jstests_error1.drop();

// test 1

db.runCommand({insert: "x", documents: [{_id: 1, a: "xx"}, {_id: 1, b: "xx"}]});
assert(db.runCommand({getlasterror: 1}).err != null, "D");

assert(db.getLastError() != null, "F");

db.jstests_error1.findOne();
assert(db.runCommand({getlasterror: 1}).err == null, "H");

db.jstests_error1.findOne();
assert(db.runCommand({getlasterror: 1}).err == null, "K");

db.forceError();
db.jstests_error1.findOne();
assert(db.getLastError() == null, "getLastError 5");
