// Tests bulk insert of docs from the shell

var coll = db.bulkInsertTest;
coll.drop();

var seed = new Date().getTime();
Random.srand(seed);
print("Seed for randomized test is " + seed);

var bulkSize = Math.floor(Random.rand() * 200) + 1;
var numInserts = Math.floor(Random.rand() * 300) + 1;

print("Inserting " + numInserts + " bulks of " + bulkSize + " documents.");

for (var i = 0; i < numInserts; i++) {
    var bulk = [];
    for (var j = 0; j < bulkSize; j++) {
        bulk.push({hi: "there", i: i, j: j});
    }

    coll.insert(bulk);
}

assert.eq(coll.count(), bulkSize * numInserts);
