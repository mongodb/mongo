// Ensure that the api versions metric does not grow indefinitely and cause the server to crash.
// Currently it is bounded by 'KMaxNumOfOutputAppNames = 1000' app names.
(function() {
"use strict";

const KMaxNumOfOutputAppNames = 1000;
const conn = MongoRunner.runMongod({verbose: 0});
const uri = "mongodb://" + conn.host + "/test";

let addAppName = function(suffix) {
    try {
        // The client.application.name cannot exceed 128 bytes.
        const appName = "a".repeat(123) + "_" + suffix;
        const newConn = new Mongo(uri + "?appName=" + appName);
        const db = newConn.getDB("test");
        db.runCommand({ping: 1});
        newConn.close();
    } catch (e) {
        // The connection may fail to be established due to the presence of too many open
        // connections.
    }
};

const db = new Mongo(uri + "?appName=ServerStatus").getDB("test");
let getNumberofAppNamesReturned = function() {
    return Object.keys(db.serverStatus().metrics.apiVersions).length;
};

for (var i = 0; i < 2000; i++) {
    addAppName(i);
    assert(getNumberofAppNamesReturned() <= KMaxNumOfOutputAppNames);
}

MongoRunner.stopMongod(conn);
})();
