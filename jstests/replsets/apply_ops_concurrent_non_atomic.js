/**
 * This test ensures that multiple non-atomic applyOps commands can run concurrently.
 * Prior to SERVER-29802, applyOps would acquire the global lock regardless of the
 * atomicity of the operations (as a whole) being applied.
 */
(function() {
    'use strict';

    /**
     * Creates an array of insert operations for applyOps into collection 'coll'.
     */
    function generateInsertOps(coll, numOps) {
        // Explicit 'use strict' to prevent mozjs from injecting its own "use strict" directive
        // (with incorrect indentation) when we convert this function into a string for
        // startParallelShell().
        'use strict';
        const ops = Array(numOps).fill('ignored').map((unused, i) => {
            return {op: 'i', ns: coll.getFullName(), o: {_id: i}};
        });
        return ops;
    }

    /**
     * Runs applyOps in non-atomic mode to insert 'numOps' documents into collection 'coll'.
     */
    function applyOpsInsertNonAtomic(coll, numOps) {
        'use strict';
        const ops = generateInsertOps(coll, numOps);
        const mydb = coll.getDB();
        assert.commandWorked(mydb.runCommand({applyOps: ops, allowAtomic: false}),
                             'failed to insert documents into ' + coll.getFullName());
    }

    /**
     * Creates a function for startParallelShell() to run that will insert documents into collection
     * 'coll' using applyOps.
     */
    function createInsertFunction(coll, numOps) {
        const options = {
            dbName: coll.getDB().getName(),
            collName: coll.getName(),
            numOps: numOps,
        };
        const functionName = 'insertFunction_' + coll.getFullName().replace(/\./g, '_');
        const insertFunction = function(options) {
            'use strict';

            const mydb = db.getSiblingDB(options.dbName);
            const coll = mydb.getCollection(options.collName);
            const numOps = options.numOps;

            jsTestLog('Starting to apply ' + numOps + ' operations in collection ' +
                      coll.getFullName());
            applyOpsInsertNonAtomic(coll, numOps);
            jsTestLog('Successfully applied ' + numOps + ' operations in collection ' +
                      coll.getFullName());
        };
        const s =                                                                     //
            '\n\n' +                                                                  //
            'const generateInsertOps = ' + generateInsertOps + ';\n\n' +              //
            'const applyOpsInsertNonAtomic = ' + applyOpsInsertNonAtomic + ';\n\n' +  //
            'const ' + functionName + ' = ' + insertFunction + ';\n\n' +              //
            functionName + '(' + tojson(options) + ');';                              //
        return s;
    }

    /**
     * Returns number of insert operations reported by serverStatus.
     * Depending on the server version, applyOps may increment either 'opcounters' or
     * 'opcountersRepl':
     *     since 3.6: 'opcounters.insert'
     *     3.4 and older: 'opcountersRepl.insert'
     */
    function getInsertOpCount(serverStatus) {
        return (serverStatus.version.substr(0, 3) === "3.4") ? serverStatus.opcountersRepl.insert
                                                             : serverStatus.opcounters.insert;
    }

    const replTest = new ReplSetTest({nodes: 1});
    replTest.startSet();
    replTest.initiate();

    const primary = replTest.getPrimary();
    const adminDb = primary.getDB('admin');
    const db1 = primary.getDB('test1');
    const coll1 = db1.coll1;
    const db2 = primary.getDB('test2');
    const coll2 = db2.coll2;

    assert.commandWorked(db1.createCollection(coll1.getName()));
    assert.commandWorked(db2.createCollection(coll2.getName()));

    // Enable fail point to pause applyOps between operations.
    assert.commandWorked(primary.adminCommand(
        {configureFailPoint: 'applyOpsPauseBetweenOperations', mode: 'alwaysOn'}));

    // This logs each operation being applied.
    const previousLogLevel =
        assert.commandWorked(primary.setLogLevel(3, 'replication')).was.replication.verbosity;

    jsTestLog('Applying operations in collections ' + coll1.getFullName() + ' and ' +
              coll2.getFullName());

    const numOps = 100;
    const insertProcess1 =
        startParallelShell(createInsertFunction(coll1, numOps), replTest.getPort(0));
    const insertProcess2 =
        startParallelShell(createInsertFunction(coll2, numOps), replTest.getPort(0));

    // The fail point will prevent applyOps from advancing past the first operation in each batch of
    // operations. If applyOps is applying both sets of operations concurrently without holding the
    // global lock, the insert opcounter will eventually be incremented to 2.
    try {
        let insertOpCount = 0;
        const expectedFinalOpCount = 2;
        assert.soon(
            function() {
                const serverStatus = adminDb.serverStatus();
                insertOpCount = getInsertOpCount(serverStatus);
                // This assertion may fail if the fail point is not implemented correctly within
                // applyOps. This allows us to fail fast instead of waiting for the assert.soon()
                // function to time out.
                assert.lte(insertOpCount,
                           expectedFinalOpCount,
                           'Expected at most ' + expectedFinalOpCount +
                               ' documents inserted with fail point enabled. ' +
                               'Most recent insert operation count = ' + insertOpCount);
                return insertOpCount === expectedFinalOpCount;
            },
            'Insert operation count did not reach ' + expectedFinalOpCount +
                ' as expected with fail point enabled. Most recent insert operation count = ' +
                insertOpCount);
    } finally {
        assert.commandWorked(primary.adminCommand(
            {configureFailPoint: 'applyOpsPauseBetweenOperations', mode: 'off'}));
    }

    insertProcess1();
    insertProcess2();

    jsTestLog('Successfully applied operations in collections ' + coll1.getFullName() + ' and ' +
              coll2.getFullName());

    // Reset log level.
    primary.setLogLevel(previousLogLevel, 'replication');

    const serverStatus = adminDb.serverStatus();
    assert.eq(200,
              getInsertOpCount(serverStatus),
              'incorrect number of insert operations in server status after applyOps: ' +
                  tojson(serverStatus));

    replTest.stopSet();
})();
