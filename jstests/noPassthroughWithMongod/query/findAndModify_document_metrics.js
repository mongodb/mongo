// Verify that findAndModify command increments document metrics correctly. These metrics only exist
// on shard.

const coll = db.find_and_modify_document_metrics;
coll.drop();
assert.commandWorked(coll.insert([{a: 1, b: 1}]));

// Update and return no document
let serverStatusBeforeTest = db.serverStatus();
let result = coll.findAndModify({query: {a: 2}, update: {$set: {b: 2}}});
let serverStatusAfterTest = db.serverStatus();
assert.eq(serverStatusBeforeTest.metrics.document.updated,
          serverStatusAfterTest.metrics.document.updated,
          `Before:  ${tojson(serverStatusBeforeTest.metrics.document)}, after: ${
              tojson(serverStatusAfterTest.metrics.document)}`);
assert.eq(serverStatusBeforeTest.metrics.document.returned,
          serverStatusAfterTest.metrics.document.returned,
          `Before:  ${tojson(serverStatusBeforeTest.metrics.document)}, after: ${
              tojson(serverStatusAfterTest.metrics.document)}`);

// Update and return a single document
serverStatusBeforeTest = db.serverStatus();
result = coll.findAndModify({query: {a: 1}, update: {$set: {b: 2}}});
serverStatusAfterTest = db.serverStatus();
assert.eq(serverStatusBeforeTest.metrics.document.updated + 1,
          serverStatusAfterTest.metrics.document.updated,
          `Before:  ${tojson(serverStatusBeforeTest.metrics.document)}, after: ${
              tojson(serverStatusAfterTest.metrics.document)}`);
assert.eq(serverStatusBeforeTest.metrics.document.returned + 1,
          serverStatusAfterTest.metrics.document.returned,
          `Before:  ${tojson(serverStatusBeforeTest.metrics.document)}, after: ${
              tojson(serverStatusAfterTest.metrics.document)}`);

// Delete and return no document
serverStatusBeforeTest = db.serverStatus();
result = coll.findAndModify({query: {a: 2}, remove: true});
serverStatusAfterTest = db.serverStatus();
assert.eq(serverStatusBeforeTest.metrics.document.deleted,
          serverStatusAfterTest.metrics.document.deleted,
          `Before:  ${tojson(serverStatusBeforeTest.metrics.document)}, after: ${
              tojson(serverStatusAfterTest.metrics.document)}`);
assert.eq(serverStatusBeforeTest.metrics.document.returned,
          serverStatusAfterTest.metrics.document.returned,
          `Before:  ${tojson(serverStatusBeforeTest.metrics.document)}, after: ${
              tojson(serverStatusAfterTest.metrics.document)}`);

// Delete and return a single document
serverStatusBeforeTest = db.serverStatus();
result = coll.findAndModify({query: {a: 1}, remove: true});
serverStatusAfterTest = db.serverStatus();
assert.eq(serverStatusBeforeTest.metrics.document.deleted + 1,
          serverStatusAfterTest.metrics.document.deleted,
          `Before:  ${tojson(serverStatusBeforeTest.metrics.document)}, after: ${
              tojson(serverStatusAfterTest.metrics.document)}`);
assert.eq(serverStatusBeforeTest.metrics.document.returned + 1,
          serverStatusAfterTest.metrics.document.returned,
          `Before:  ${tojson(serverStatusBeforeTest.metrics.document)}, after: ${
              tojson(serverStatusAfterTest.metrics.document)}`);
