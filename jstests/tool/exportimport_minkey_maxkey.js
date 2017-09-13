var tt = new ToolTest('exportimport_minkey_maxkey_test');

var exportimport_db = tt.startDB();

var src = exportimport_db.src;
var dst = exportimport_db.dst;

src.drop();
dst.drop();

src.insert({"_id": MaxKey});
src.insert({"_id": MinKey});

print('About to call mongoexport on: ' + exportimport_db.getName() + '.' + src.getName() +
      ' with file: ' + tt.extFile);
tt.runTool('export', '--out', tt.extFile, '-d', exportimport_db.getName(), '-c', src.getName());

print('About to call mongoimport on: ' + exportimport_db.getName() + '.' + dst.getName() +
      ' with file: ' + tt.extFile);
tt.runTool('import', '--file', tt.extFile, '-d', exportimport_db.getName(), '-c', dst.getName());

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
