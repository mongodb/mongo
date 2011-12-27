// helpers for testing repl sets
// run
//   mongo --shell <host:port> testing.js

cfg = {
    _id: 'asdf',
    members: [
        { _id : 0, host : "dm_hp" },
        { _id : 2, host : "dm_hp:27002" }
        ]
};
c2 = {
    _id: 'asdf',
    members: [
        { _id: 0, host: "dmthink" },
        { _id: 2, host: "dmthink:27002" }
        ]
};

db = db.getSisterDB("admin");
local = db.getSisterDB("local");

print("\n\ndb = admin db on localhost:27017");
print("b = admin on localhost:27002");
print("rc(x) = db.runCommand(x)");
print("cfg = samp replset config");
print("i() = replSetInitiate(cfg)");
print("ism() = rc('ismaster')");
print("\n\n");

function rc(c) { return db.runCommand(c); }
function i() { return rc({ replSetInitiate: cfg }); }
function ism() { return rc("isMaster"); }

b = 0;
try {
    b = new Mongo("localhost:27002").getDB("admin");
}
catch (e) {
    print("\nCouldn't connect to b mongod instance\n");
}

