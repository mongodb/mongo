// perform basic js tests in parallel

load('jstests/libs/parallelTester.js');

Random.setRandomSeed();

var params = ParallelTester.createJstestsLists(4);
var t = new ParallelTester();
for (i in params) {
    t.add(ParallelTester.fileTester, params[i]);
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
