// Test that the --throttleBPS option of mongoimport works
var t = new ToolTest("exportimport_throttle");

var d = t.startDB();

var src_c = d.src;
var dst_c = d.dst;

src_c.drop();
dst_c.drop();

// Calculate the number of documents it takes to get above 16MB (here using 20MB just to be safe)
var bigString = new Array(1025).toString();
var doc = {_id: new ObjectId(), x:bigString};
var docSize = Object.bsonsize(doc);
var targetColSize = 10 * 1024 * 1024;
var numDocs = Math.floor(targetColSize / docSize);

print("Inserting " + numDocs + " documents into " + d.getName() + "." + src_c.getName() + 
      "to make a ~10Mb collection")
var i;
for (i = 0; i < numDocs; ++i) {
    src_c.insert({ x : bigString });
}
var lastError = d.getLastError();
if (lastError == null) {
    print("Finished inserting " + numDocs + " documents");
}
else {
    doassert("Insertion failed: " + lastError);
}

var targetTimeSecs = 10; //secs
var testThrottleBps = targetColSize / targetTimeSecs;
//Allowing +/- 3 secs for buffering rate flow changes, and furthermore +/- 10% in the accuracy
//  of the flow rate
var minRunTimeMs = (0.9 * (targetTimeSecs - 3)) * 1000 /*ms*/;
var maxRunTimeMs = (1.1 * (targetTimeSecs + 3)) * 1000 /*ms*/;

// Do export and import of ordinary (i.e. not jsonArray-type) collection
print("About to call mongoexport on: " + d.getName() + "." + src_c.getName());
t.runTool("export", "--out" , t.extFile, "-d", d.getName(), "-c", src_c.getName());

print("About to mongoimport to: " + d.getName() + "." + dst_c.getName() + " throttled to " + 
      testThrottleBps + " bytes per second");
var importStartDt = new Date();
t.runTool("import", "--file", t.extFile, "-d", d.getName(), "-c", dst_c.getName(),
           "--throttleBPS", testThrottleBps.toString());
var importTimeMs = new Date() - importStartDt;
print("import time was " + (importTimeMs / 1000) + " secs");

assert.eq(src_c.count(), dst_c.count(), "imported collection " + dst_c.getName() + " did not have an " + 
          "equal count to the source collection " + src_c.getName());

//Allowing +/- 3 secs for buffering rate flow changes, and furthermore +/- 10% in the accuracy
//  of the flow rate
//N.b. because the data is small external performance issues are not expected to limit the speed. 
//  If assumption turns out to be incorrect remove the test against min time.
var minRunTimeMs = (0.9 * (targetTimeSecs - 3)) * 1000 /*ms*/;
var maxRunTimeMs = (1.1 * (targetTimeSecs + 3)) * 1000 /*ms*/;
assert(importTimeMs > minRunTimeMs && importTimeMs < maxRunTimeMs, "import went too " + 
       (importTimeMs < minRunTimeMs ? "slow" : "fast") + "- " + (importTimeMs / 1000) + " secs, " + 
       "not close enough to ideal " + targetTimeSecs + " secs");

dst_c.drop();
removeFile(t.extFile);

// Do export and import of jsonArray-type collection
print("About to call mongoexport --jsonArray on: " + d.getName() + "." + src_c.getName()); 
t.runTool("export", "--out" , t.extFile, "-d", d.getName(), "-c", src_c.getName(), "--jsonArray");

print("About to mongoimporta `jsonArray-type file to: " + d.getName() + "." + dst_c.getName() +
      " throttled to " + testThrottleBps + " bytes per second");
var importStartDt = new Date();
t.runTool("import", "--file", t.extFile, "-d", d.getName(), "-c", dst_c.getName(),
           "--jsonArray", "--throttleBPS", testThrottleBps.toString());
var importTimeMs = new Date() - importStartDt;
print("import jsonArray time was " + (importTimeMs / 1000) + " secs");

assert.eq(src_c.count(), dst_c.count(), "imported collection " + dst_c.getName() + " did not have an " + 
          "equal count to the source collection " + src_c.getName());

//N.b. disabled the check that it's running fast enough.
assert(/*importTimeMs > minRunTimeMs &&*/ importTimeMs < maxRunTimeMs, "import of jsonArray data " +
       "went too " + (importTimeMs < minRunTimeMs ? "slow" : "fast") + "- " + 
       (importTimeMs / 1000) + " secs, " + "not close enough to ideal " + targetTimeSecs + " secs");

src_c.drop();
dst_c.drop();
removeFile(t.extFile);

t.stop();
