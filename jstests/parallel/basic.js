// perform basic js tests in parallel
load('jstests/libs/parallelTester.js');

Random.setRandomSeed();

var params = ParallelTester.createJstestsLists(4);
var t = new ParallelTester();
for (i in params) {
    t.add(ParallelTester.fileTester, params[i]);
}

t.run("one or more tests failed", true);

db.getCollectionNames().forEach(function(x) {
    v = db[x].validate();
    assert(v.valid, "validate failed for " + x + " with " + tojson(v));
});