// Testing parallelism of mapReduce in V8

// Our server and client need to be running V8 and the host we are running on needs at least two
// cores.  Update this if you are testing more than three threads in parallel.
if (/V8/.test(interpreterVersion()) && db.runCommand({buildinfo: 1}).javascriptEngine == "V8" &&
    db.hostInfo().system.numCores >= 2) {
    // function timeSingleThread
    // Description: Gathers data about how long it takes to run a given job
    // Args: job - job to run
    //       tid - thread id passed as an argument to the job, default 0
    // Returns: { threadStart : <time job started> , threadEnd : <time job completed> }
    var timeSingleThread = function(job, tid) {
        var tid = tid || 0;
        var threadStart = new Date();
        job(tid);
        return {"threadStart": threadStart, "threadEnd": new Date()};
    };

    // function timeMultipleThreads
    // Description: Gathers data about how long it takes to run a given job in multiple threads.
    // Args: job - job to run in each thread
    // nthreads - number of threads to spawn
    // stagger - delay between each thread spawned in milliseconds
    // Returns: Array with one entry for each thread of the form:
    // [ { threadStart : <time elapsed before thread started work> ,
    //     threadEnd : <time elapsed before thread completed work> } ,
    //     ...
    //     ]
    var timeMultipleThreads = function(job, nthreads, stagger) {
        var i = 0;
        var threads = [];

        for (i = 0; i < nthreads; ++i) {
            threads[i] = new Thread(timeSingleThread, job, i);
        }

        // Our "reference time" that all threads agree on
        var referenceTime = new Date();

        for (i = 0; i < nthreads; ++i) {
            if (stagger && i > 0) {
                sleep(stagger);
            }
            threads[i].start();
        }

        var threadTimes = [];
        for (i = 0; i < nthreads; ++i) {
            var returnData = threads[i].returnData();
            threadTimes[i] = {};
            threadTimes[i].threadStart = returnData.threadStart - referenceTime;
            threadTimes[i].threadEnd = returnData.threadEnd - referenceTime;
        }

        return threadTimes;
    };

    // Display and analysis helper functions

    var getLastCompletion = function(threadTimes) {
        var lastCompletion = 0;
        for (var i = 0; i < threadTimes.length; i++) {
            lastCompletion = Math.max(lastCompletion, threadTimes[i].threadEnd);
        }
        return lastCompletion;
    };

    // Functions we are performance testing

    db.v8_parallel_mr_src.drop();

    for (j = 0; j < 100; j++)
        for (i = 0; i < 512; i++) {
            db.v8_parallel_mr_src.save({j: j, i: i});
        }

    db.getLastError();

    var mrWorkFunction = function() {

        function verifyOutput(out) {
            // printjson(out);
            assert.eq(out.counts.input, 51200, "input count is wrong");
            assert.eq(out.counts.emit, 51200, "emit count is wrong");
            assert.gt(out.counts.reduce, 99, "reduce count is wrong");
            assert.eq(out.counts.output, 512, "output count is wrong");
        }

        function map() {
            if (this.j % 2 == 0) {
                emit(this.i, this.j * this.j);
            } else {
                emit(this.i, this.j + this.j);
            }
        }

        function reduce(key, values) {
            values_halved = values.map(function(value) {
                return value / 2;
            });
            values_halved_sum = Array.sum(values_halved);
            return values_halved_sum;
        }

        var out = db.v8_parallel_mr_src.mapReduce(map, reduce, {out: "v8_parallel_mr_out"});
        verifyOutput(out);
    };

    var oneMapReduce = getLastCompletion(timeMultipleThreads(mrWorkFunction, 1));
    var twoMapReduce = getLastCompletion(timeMultipleThreads(mrWorkFunction, 2));
    var threeMapReduce = getLastCompletion(timeMultipleThreads(mrWorkFunction, 3));

    printjson("One map reduce job: " + oneMapReduce);
    printjson("Two map reduce jobs: " + twoMapReduce);
    printjson("Three map reduce jobs: " + threeMapReduce);

    assert(oneMapReduce * 1.75 > twoMapReduce);
    assert(oneMapReduce * 2.5 > threeMapReduce);
}
