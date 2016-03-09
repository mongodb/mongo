// Tests serverStatus tracking of connections opened

var numPerTypeToCreate = 50;

// We need to create a new mongod to ensure no one else is talking to us in the background, and
// will screw up our stats.
var mongo = MongoRunner.runMongod({});
var db = mongo.getDB("test");

var availableConnections = db.serverStatus().connections.available;
if (availableConnections < (numPerTypeToCreate * 10)) {
    numPerTypeToCreate = Math.floor(availableConnections / 10);
}

print("numPerTypeToCreate: " + numPerTypeToCreate);

var testDB = 'connectionsOpenedTest';
var signalCollection = 'keepRunning';

function createPersistentConnection() {
    assert.soon(function() {
        try {
            return new Mongo(db.getMongo().host);
        } catch (x) {
            return false;
        }
    }, "Timed out waiting for persistent connection to connect", 30000, 5000);
}

function createTemporaryConnection() {
    // Retry connecting until you are successful
    var pollString = "var conn = null;" + "assert.soon(function() {" + "try { conn = new Mongo(\"" +
        db.getMongo().host + "\"); return conn" + "} catch (x) {return false;}}, " +
        "\"Timed out waiting for temporary connection to connect\", 30000, 5000);";
    // Poll the signal collection until it is told to terminate.
    pollString += "assert.soon(function() {" + "return conn.getDB('" + testDB +
        "').getCollection('" + signalCollection + "')" +
        ".findOne().stop;}, \"Parallel shell never told to terminate\", 10 * 60000);";
    return startParallelShell(pollString, null, true);
}

function waitForConnections(expectedCurrentConnections, expectedTotalConnections) {
    assert.soon(
        function() {
            var currentConnInfo = db.serverStatus().connections;
            return (expectedCurrentConnections == currentConnInfo.current) &&
                (expectedTotalConnections, currentConnInfo.totalCreated);
        },
        {
          toString: function() {
              return "Incorrect connection numbers. Expected " + expectedCurrentConnections +
                  " current connections and " + expectedTotalConnections + " total" +
                  " connections. Connection info from serverStatus: " +
                  tojson(db.serverStatus().connections);
          }
        },
        5 * 60000);
}

var originalConnInfo = db.serverStatus().connections;
assert.gt(originalConnInfo.current, 0);
assert.gt(originalConnInfo.totalCreated, 0);

jsTestLog("Creating persistent connections");
var permConns = [];
for (var i = 0; i < numPerTypeToCreate; i++) {
    permConns.push(createPersistentConnection());
}

jsTestLog("Testing that persistent connections increased the current and totalCreated counters");
waitForConnections(originalConnInfo.current + numPerTypeToCreate,
                   originalConnInfo.totalCreated + numPerTypeToCreate);

jsTestLog("Creating temporary connections");
db.getSiblingDB(testDB).dropDatabase();
db.getSiblingDB(testDB).getCollection(signalCollection).insert({stop: false});

var tempConns = [];
for (var i = 0; i < numPerTypeToCreate; i++) {
    tempConns.push(createTemporaryConnection());
}

jsTestLog("Testing that temporary connections increased the current and totalCreated counters");
waitForConnections(originalConnInfo.current + numPerTypeToCreate * 2,
                   originalConnInfo.totalCreated + numPerTypeToCreate * 2);

jsTestLog("Waiting for all temporary connections to be closed");
// Notify waiting parallel shells to terminate, causing the connection count to go back down.
db.getSiblingDB(testDB).getCollection(signalCollection).update({}, {$set: {stop: true}});
for (var i = 0; i < tempConns.length; i++) {
    tempConns[i]();  // wait on parallel shell to terminate
}

jsTestLog("Testing that current connections counter went down after temporary connections closed");
waitForConnections(originalConnInfo.current + numPerTypeToCreate,
                   originalConnInfo.totalCreated + numPerTypeToCreate * 2);

persistent = null;
gc();

MongoRunner.stopMongod(mongo);
