// Tests serverStatus tracking of connections opened

var testDB = 'connectionsOpenedTest';
var signalCollection = 'keepRunning';

function createPersistentConnection() {
    new Mongo(db.getMongo().host);
}

function createTemporaryConnection() {
    // Creates a connection by spawing a shell that polls the signal collection until it is told to
    // terminate.
    var pollString = "assert.soon(function() {"
        + "return db.getSiblingDB('" + testDB + "').getCollection('" + signalCollection + "')"
        + ".findOne().stop;});";
    return startParallelShell(pollString);
}


var originalConnInfo = db.serverStatus().connections;
assert.gt(originalConnInfo.current, 0);
assert.gt(originalConnInfo.totalCreated, 0);

jsTestLog("Creating persistent connections");
for (var i = 0; i < 100; i++) {
    createPersistentConnection();
}

jsTestLog("Testing that persistent connections increased the current and totalCreated counters");
var currentConnInfo = db.serverStatus().connections;
assert.eq(originalConnInfo.current + 100, currentConnInfo.current);
assert.eq(originalConnInfo.totalCreated + 100, currentConnInfo.totalCreated);

jsTestLog("Creating temporary connections");
db.getSiblingDB(testDB).dropDatabase();
db.getSiblingDB(testDB).getCollection(signalCollection).insert({stop:false});

var tempConns = [];
for (var i = 0; i < 100; i++) {
    tempConns.push(createTemporaryConnection());
}

jsTestLog("Testing that temporary connections increased the current and totalCreated counters");
assert.soon(function() {
                currentConnInfo = db.serverStatus().connections;
                return (originalConnInfo.current + 201 == currentConnInfo.current) &&
                    (originalConnInfo.totalCreated + 200, currentConnInfo.totalCreated);
            },
           {toString: function() {
                return "Temp connections didn't increase connection counters."
                       + " serverStatus.connections: " + tojson(db.serverStatus().connections); } });

jsTestLog("Waiting for all temporary connections to be closed");
// Notify waiting parallel shells to terminate, causing the connection count to go back down.
db.getSiblingDB(testDB).getCollection(signalCollection).update({}, {$set : {stop:true}});
for (var i = 0; i < tempConns.length; i++) {
    tempConns[i](); // wait on parallel shell to terminate
}

jsTestLog("Testing that current connections counter went down after temporary connections closed");
currentConnInfo = db.serverStatus().connections;
assert.eq(originalConnInfo.current + 100, currentConnInfo.current);
assert.eq(originalConnInfo.totalCreated + 200, currentConnInfo.totalCreated);
