// perform inserts in parallel from a large number of clients
load('jstests/libs/parallelTester.js');

f = db.jstests_parallel_manyclients;
f.drop();
f.ensureIndex({who: 1});

Random.setRandomSeed();

t = new ParallelTester();

// Reducing the number of threads to 100 because of WT-1989
numThreads = 100;
buildInfo = db.adminCommand("buildInfo");

if (buildInfo.bits < 64 || buildInfo.buildEnvironment.target_os != "linux" || buildInfo.debug) {
    numThreads = 50;
}

numThreads = Math.min(numThreads, db.serverStatus().connections.available / 3);

print("numThreads: " + numThreads);

for (id = 0; id < numThreads; ++id) {
    var g = new EventGenerator(id, "jstests_parallel_manyclients", Random.randInt(20));
    for (j = 0; j < 1000; ++j) {
        if (j % 50 == 0) {
            g.addCheckCount(j, {who: id}, false);
        }
        g.addInsert({i: j, who: id});
    }
    t.add(EventGenerator.dispatch, g.getEvents());
}

print("done preparing test");

t.run("one or more tests failed");

assert(f.validate().valid);
