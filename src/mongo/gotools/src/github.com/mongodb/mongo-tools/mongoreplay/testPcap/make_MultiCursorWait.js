db1 = (new Mongo("localhost:27017")).getDB("mongoreplay");

var toInsert = [];
for (var j = 0; j < 10000; j++) {
    toInsert.push({
        x: j
    });
}

for (var i = 0; i < 100; i++) {
    db1.test.insert(toInsert);
}
db2 = (new Mongo("localhost:27017")).getDB("mongoreplay");

var agg1 = db2.runCommand({
    aggregate: "test",
    cursor: {
        batchSize: 5
    }
});

cursorId = agg1.cursor.id;

print("got cursor fron cxn1: ", cursorId);

var cursor1 = db2.getMongo().cursorFromId("mongoreplay.test", cursorId);

while (cursor1.hasNext()) {
    printjson(cursor1.next());
}

var agg2 = db1.runCommand({
    aggregate: "test",
    cursor: {
        batchSize: 5
    }
});

var cursor2 = db1.getMongo().cursorFromId("mongoreplay.test", agg2.cursor.id);
