// Test importing collections represented as a single line array above the maximum document size
var tt = new ToolTest('exportimport_bigarray_test');

var exportimport_db = tt.startDB();

var src = exportimport_db.src;
var dst = exportimport_db.dst;

src.drop();
dst.drop();

// Calculate the number of documents it takes to get above 16MB (here using 20MB just to be safe)
var bigString = new Array(1025).toString();
var doc = {_id: new ObjectId(), x: bigString};
var docSize = Object.bsonsize(doc);
var numDocs = Math.floor(20 * 1024 * 1024 / docSize);

print('Size of one document: ' + docSize);
print('Number of documents to exceed maximum BSON size: ' + numDocs);

print('About to insert ' + numDocs + ' documents into ' + exportimport_db.getName() + '.' +
      src.getName());
var i;
var bulk = src.initializeUnorderedBulkOp();
for (i = 0; i < numDocs; ++i) {
    bulk.insert({x: bigString});
}
assert.writeOK(bulk.execute());

data = 'data/exportimport_array_test.json';

print('About to call mongoexport on: ' + exportimport_db.getName() + '.' + src.getName() +
      ' with file: ' + data);
tt.runTool(
    'export', '--out', data, '-d', exportimport_db.getName(), '-c', src.getName(), '--jsonArray');

print('About to call mongoimport on: ' + exportimport_db.getName() + '.' + dst.getName() +
      ' with file: ' + data);
tt.runTool(
    'import', '--file', data, '-d', exportimport_db.getName(), '-c', dst.getName(), '--jsonArray');

print('About to verify that source and destination collections match');

src_cursor = src.find().sort({_id: 1});
dst_cursor = dst.find().sort({_id: 1});

var documentCount = 0;
while (src_cursor.hasNext()) {
    assert(dst_cursor.hasNext(),
           'Source has more documents than destination. ' +
               'Destination has ' + documentCount + ' documents.');
    assert.eq(src_cursor.next(), dst_cursor.next(), 'Mismatch on document ' + documentCount);
    ++documentCount;
}
assert(!dst_cursor.hasNext(),
       'Destination has more documents than source. ' +
           'Source has ' + documentCount + ' documents.');

print('Verified that source and destination collections match');
