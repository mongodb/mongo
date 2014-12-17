// SERVER-15319 Make sure pdfile version bumping for the new freelist works as intended

var tooOld = "2.4"; // anything <= 2.6.4
var newEnough = "2.6"; // latest in 2.6 is >= 2.6.5
var latest = "latest";

function restart(conn, version) {
    MongoRunner.stopMongod(conn);
    return MongoRunner.runMongod({restart: conn, remember: true, binVersion: version});
}

function repair(conn, version) {
    MongoRunner.stopMongod(conn);
    MongoRunner.runMongod({restart: conn, remember: true, binVersion: version, repair: ""});
}


// start out running tooOld
var conn = MongoRunner.runMongod({ remember: true, binVersion: tooOld, smallfiles: "" });
assert(conn);

// can go from tooOld to newEnough
conn = restart(conn, newEnough);
assert(conn);

// can go from newEnough to tooOld
conn = restart(conn, tooOld);
assert(conn);

// can go from tooOld to latest
conn = restart(conn, latest);
assert(conn);

// can go from latest to newEnough
conn = restart(conn, newEnough);
assert(conn);

// can't go back to tooOld once started with latest, even if started with newEnough in between.
assert.isnull(restart(conn, tooOld));

// can still start with newEnough
conn = restart(conn, newEnough);
assert(conn);

// can still start with latest
conn = restart(conn, latest);
assert(conn);

// can repair with newEnough
repair(conn, newEnough);

// can start with tooOld after repairing with newEnough
conn = restart(conn, tooOld);
assert(conn);

// THIS MUST BE THE LAST LINE
MongoRunner.stopMongod(conn);
