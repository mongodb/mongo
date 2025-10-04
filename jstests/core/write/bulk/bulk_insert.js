// Tests bulk insert of docs from the shell
//
// @tags: [requires_fastcount]

let coll = db.bulkInsertTest;
coll.drop();

let seed = new Date().getTime();
Random.srand(seed);
print("Seed for randomized test is " + seed);

let bulkSize = Math.floor(Random.rand() * 200) + 1;
let numInserts = Math.floor(Random.rand() * 300) + 1;

print("Inserting " + numInserts + " bulks of " + bulkSize + " documents.");

for (let i = 0; i < numInserts; i++) {
    let bulk = [];
    for (let j = 0; j < bulkSize; j++) {
        bulk.push({hi: "there", i: i, j: j});
    }

    coll.insert(bulk);
}

assert.eq(coll.count(), bulkSize * numInserts);
