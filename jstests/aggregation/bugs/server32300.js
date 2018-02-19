// Core ServerSERVER-32300 Aggregation shell helper should not modify options document

load("src/mongo/shell/explainable.js")

let collation = {collation: {locale: "fr", backwards: true}};

// Computes correct result of the aggragation
// Computes an empty array
jsTestLog("desired output: " + tojson(db.collection.aggregate([], collation).toArray()));
jsTestLog(tojson(collation));

// Computes the explian output
jsTestLog("explain output: " + tojson(db.collection.explain().aggregate([], collation)));
jsTestLog(tojson(collation));

// Originally, the option document in this test was modified by previous call (expalin function), it computed the explain output, but instead of the resut of the aggregation.
// The correct output should be an empty array
jsTestLog("actual output when explain interspers between: " + tojson(db.collection.aggregate([], collation).toArray()));
jsTestLog(tojson(collation));
