/*
 * Test that the 'setProfilingFilterGlobally' command runs concurrently without crashes.
 *
 * @tags: [
 *   uses_parallel_shell,
 * ]
 */
(function() {

const conn = MongoRunner.runMongod({setParameter: {internalQueryGlobalProfilingFilter: 1}});

let handles = [];

const numThreads = 5;
for (let i = 0; i < numThreads; ++i) {
    handles.push(startParallelShell(() => {
        const numIterations = 500;
        for (let j = 0; j < numIterations; ++j) {
            const res = assert.commandWorked(db.runCommand(
                {setProfilingFilterGlobally: 1, filter: j % 2 == 0 ? {nreturned: 0} : "unset"}));
            assert(res.hasOwnProperty("was"), tojson(res));
        }
    }, conn.port));
}

for (const handle of handles) {
    handle();
}

MongoRunner.stopMongod(conn);
})();
