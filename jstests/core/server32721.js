// This file tecnds to make expalin to use backup plan, just for testing
db.foo.drop()

let bulk = db.foo.initializeUnorderedBulkOp();
for (let i = 0; i < 100000; ++i) {
  bulk.insert({_id: i, x: i, y: i});
}

bulk.execute();
db.foo.ensureIndex({x: 1})

//2. Configure log level and lower the sort bytes limit:
db.setLogLevel(5, "query");
db.adminCommand({setParameter: 1, internalQueryExecMaxBlockingSortBytes: 100});

//3. Run the query that will have lots of matching documents that are not sorted from the x index, and lots of non-matching documents that are sorted from the _id index:
db.foo.find({x: {$gte: 90000}}).sort({_id: 1}).explain(true)
