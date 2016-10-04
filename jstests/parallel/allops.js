// test all operations in parallel
load('jstests/libs/parallelTester.js');

f = db.jstests_parallel_allops;
f.drop();

Random.setRandomSeed();

t = new ParallelTester();

for (id = 0; id < 10; ++id) {
    var g = new EventGenerator(id, "jstests_parallel_allops", Random.randInt(20));
    for (var j = 0; j < 1000; ++j) {
        var op = Random.randInt(3);
        switch (op) {
            case 0:  // insert
                g.addInsert({_id: Random.randInt(1000)});
                break;
            case 1:  // remove
                g.addRemove({_id: Random.randInt(1000)});
                break;
            case 2:  // update
                g.addUpdate({_id: {$lt: 1000}}, {_id: Random.randInt(1000)});
                break;
            default:
                assert(false, "Invalid op code");
        }
    }
    t.add(EventGenerator.dispatch, g.getEvents());
}

var g = new EventGenerator(id, "jstests_parallel_allops", Random.randInt(5));
for (var j = 1000; j < 3000; ++j) {
    g.addCheckCount(j - 1000, {_id: {$gte: 1000}}, j % 100 == 0, j % 500 == 0);
    g.addInsert({_id: j});
}
t.add(EventGenerator.dispatch, g.getEvents());

t.run("one or more tests failed");

assert(f.validate().valid);
