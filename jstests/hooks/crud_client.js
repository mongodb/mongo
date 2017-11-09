// Basic CRUD client to provide load for Powercycle testing.

'use strict';

function randString(maxLength) {
    maxLength = maxLength || 1024;
    const randChars = "ABCDEFGHIKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    var rString = "";
    for (var i = 0; i < Random.randInt(maxLength); i++) {
        rString += randChars.charAt(Random.randInt(randChars.length));
    }
    return rString;
}

function weightedChoice(choices) {
    var total = 0;
    for (var choice in choices) {
        total += choices[choice];
    }
    var ran = Random.randInt(total);
    var upto = 0;
    for (choice in choices) {
        var weight = choices[choice];
        if (upto + weight >= ran) {
            return choice;
        }
        upto += weight;
    }
}

var operations = {
    "bulk insert": 15,
    "count": 20,
    "find": 15,
    "remove multi": 15,
    "remove one": 15,
    "upsert one": 15,
    "upsert multi": 5
};

Random.setRandomSeed();

if (typeof TestData === "undefined") {
    TestData = {};
}
var dbName = TestData.dbName || "test";
var collectionName = TestData.collectionName || "crud";
var bulkNum = TestData.bulkNum || 1000;
var baseNum = TestData.baseNum || 100000;
// Set numLoops <= 0 to have an infinite loop.
var numLoops = TestData.numLoops || 0;

print("****Starting CRUD client, namespace", dbName, collectionName, "numLoops", numLoops, "****");

var coll = db.getSiblingDB(dbName)[collectionName];
coll.createIndex({x: 1});

var shouldLoopForever = numLoops <= 0;
while (shouldLoopForever || numLoops > 0) {
    if (!shouldLoopForever) {
        numLoops -= 1;
    }

    var info = db.hostInfo();
    var serverStatus = db.serverStatus();
    print("(" + collectionName + ") dbHostInfo status:",
          info.ok,
          serverStatus.version,
          "uptime:",
          serverStatus.uptime);
    var match = Random.randInt(baseNum);
    var matchQuery = {$gte: match, $lt: match + (baseNum * 0.01)};

    var operation = weightedChoice(operations);

    if (operation == "upsert multi") {
        var updateOpts = {upsert: true, multi: true};
        print("(" + collectionName + ") Upsert multi docs",
              tojsononeline(matchQuery),
              tojsononeline(updateOpts),
              tojsononeline(coll.update(
                  {x: matchQuery}, {$inc: {x: baseNum}, $set: {n: "hello"}}, updateOpts)));
    } else if (operation == "upsert one") {
        var updateOpts = {upsert: true, multi: false};
        print("(" + collectionName + ") Upsert single doc",
              match,
              tojsononeline(updateOpts),
              tojsononeline(
                  coll.update({x: match}, {$inc: {x: baseNum}, $set: {n: "hello"}}, updateOpts)));
    } else if (operation == "bulk insert") {
        var bulk = coll.initializeUnorderedBulkOp();
        for (var i = 0; i < bulkNum; i++) {
            bulk.insert({x: (match + i) % baseNum, doc: randString()});
        }
        print(
            "(" + collectionName + ") Bulk insert", bulkNum, "docs", tojsononeline(bulk.execute()));
    } else if (operation == "count") {
        var countOpts = {count: collectionName, query: {x: matchQuery}};
        print("(" + collectionName + ") Count docs",
              tojsononeline(matchQuery),
              tojsononeline(db.runCommand(countOpts)));
    } else if (operation == "find") {
        var findOpts = {find: collectionName, singleBatch: true, filter: {x: matchQuery}};
        print("(" + collectionName + ") Find docs",
              tojsononeline(matchQuery),
              "find status",
              db.runCommand(findOpts).ok);
    } else if (operation == "remove multi") {
        var removeOpts = {};
        var removeQuery = {x: matchQuery, justOne: false};
        print("(" + collectionName + ") Remove docs",
              tojsononeline(removeQuery),
              tojsononeline(removeOpts),
              tojsononeline(coll.remove(removeQuery, removeOpts)));
    } else if (operation == "remove one") {
        var removeOpts = {};
        var removeQuery = {x: match, justOne: true};
        print("(" + collectionName + ") Remove docs",
              tojsononeline(removeQuery),
              tojsononeline(removeOpts),
              tojsononeline(coll.remove(removeQuery, removeOpts)));
    }
}
