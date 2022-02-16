// perform basic js tests in parallel & some other tasks as well
load('jstests/libs/parallelTester.js');

var c = db.jstests_parallel_basicPlus;
c.drop();

Random.setRandomSeed();

var params = ParallelTester.createJstestsLists(4);
var t = new ParallelTester();
for (i in params) {
    t.add(ParallelTester.fileTester, params[i]);
}

for (var i = 4; i < 8; ++i) {
    var g = new EventGenerator(i, "jstests_parallel_basicPlus", Random.randInt(20));
    for (var j = (i - 4) * 3000; j < (i - 3) * 3000; ++j) {
        var expected = j - ((i - 4) * 3000);
        g.addCheckCount(expected,
                        {_id: {$gte: ((i - 4) * 3000), $lt: ((i - 3) * 3000)}},
                        expected % 1000 == 0,
                        expected % 500 == 0);
        g.addInsert({_id: j});
        // Add currentOp commands running in parallel. Historically there have been many race
        // conditions between various commands and the currentOp command.
        g.addCurrentOp();
    }
    t.add(EventGenerator.dispatch, g.getEvents());
}
try {
    t.run("one or more tests failed");
} finally {
    print(
        "If the failure here is due to a test unexpected being run, " +
        "it may be due to the parallel suite not honoring feature flag tags. " +
        "If you want to skip such tests in parallel suite, " +
        "please add them to the exclusion list at " +
        "https://github.com/mongodb/mongo/blob/eb75b6ccc62f7c8ea26a57c1b5eb96a41809396a/jstests/libs/parallelTester.js#L149.");
}
