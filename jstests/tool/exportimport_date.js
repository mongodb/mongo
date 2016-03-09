var tt = new ToolTest('exportimport_date_test');

var exportimport_db = tt.startDB();

var src = exportimport_db.src;
var dst = exportimport_db.dst;

src.drop();
dst.drop();

// Insert a date that we can format
var formatable = ISODate("1970-01-02T05:00:00Z");
assert.eq(formatable.valueOf(), 104400000);
src.insert({"_id": formatable});

// Insert a date that we cannot format as an ISODate string
var nonformatable = ISODate("3001-01-01T00:00:00Z");
assert.eq(nonformatable.valueOf(), 32535216000000);
src.insert({"_id": nonformatable});

// Verify number of documents inserted
assert.eq(2, src.find().itcount());

data = 'data/exportimport_date_test.json';

print('About to call mongoexport on: ' + exportimport_db.getName() + '.' + src.getName() +
      ' with file: ' + data);
tt.runTool('export', '--out', data, '-d', exportimport_db.getName(), '-c', src.getName());

print('About to call mongoimport on: ' + exportimport_db.getName() + '.' + dst.getName() +
      ' with file: ' + data);
tt.runTool('import', '--file', data, '-d', exportimport_db.getName(), '-c', dst.getName());

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
