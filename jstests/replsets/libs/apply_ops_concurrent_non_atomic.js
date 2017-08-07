/**
 * This test ensures that multiple non-atomic applyOps commands can run concurrently.
 * Prior to SERVER-29802, applyOps would acquire the global lock regardless of the
 * atomicity of the operations (as a whole) being applied.
 *
 * Every instance of ApplyOpsConcurrentNonAtomicTest is configured with an "options" document
 * with the following format:
 * {
 *     ns1: <string>,
 *     ns1: <string>,
 *     requiresDocumentLevelConcurrency: <bool>,
 * }
 *
 * ns1:
 *     Fully qualified namespace of first set of CRUD operations. For simplicity, only insert
 *     operations will be used. The set of documents generated for the inserts into ns1 will have
 *     _id values distinct from those generated for ns2.
 *
 * ns2:
 *     Fully qualified namespace of second set of CRUD operations. This may be the same namespace as
 *     ns1. As with ns1, only insert operations will be used.
 *
 * requiresDocumentLevelConcurrency:
 *     Set to true if this test case can only be run with a storage engine that supports document
 *     level concurrency.
 */
var ApplyOpsConcurrentNonAtomicTest = function(options) {
    'use strict';

    load('jstests/concurrency/fsm_workload_helpers/server_types.js');

    if (!(this instanceof ApplyOpsConcurrentNonAtomicTest)) {
        return new ApplyOpsConcurrentNonAtomicTest(options);
    }

    // Capture the 'this' reference
    var self = this;

    self.options = options;

    /**
     * Logs message using test name as prefix.
     */
    function testLog(message) {
        jsTestLog('ApplyOpsConcurrentNonAtomicTest: ' + message);
    }

    /**
     * Creates an array of insert operations for applyOps into collection 'coll'.
     */
    function generateInsertOps(coll, numOps, id) {
        // Explicit 'use strict' to prevent mozjs from injecting its own "use strict" directive
        // (with incorrect indentation) when we convert this function into a string for
        // startParallelShell().
        'use strict';
        const ops = Array(numOps).fill('ignored').map((unused, i) => {
            return {op: 'i', ns: coll.getFullName(), o: {_id: (id * numOps + i), id: id}};
        });
        return ops;
    }

    /**
     * Runs applyOps in non-atomic mode to insert 'numOps' documents into collection 'coll'.
     */
    function applyOpsInsertNonAtomic(coll, numOps, id) {
        'use strict';
        const ops = generateInsertOps(coll, numOps, id);
        const mydb = coll.getDB();
        assert.commandWorked(mydb.runCommand({applyOps: ops, allowAtomic: false}),
                             'failed to insert documents into ' + coll.getFullName());
    }

    /**
     * Parses 'numOps' and collection namespace from 'options' and runs applyOps to inserted
     * generated documents.
     *
     * options format:
     * {
     *     ns: <string>,
     *     numOps: <int>,
     *     id: <int>,
     * }
     *
     * ns:
     *     Fully qualified namespace of collection to insert documents into.
     *
     * numOps:
     *     Number of insert operations to generate for applyOps command.
     *
     * id:
     *     Index of collection for applyOps. Used with 'numOps' to generate _id values that will not
     *     collide with collections with different indexes.
     */
    function insertFunction(options) {
        'use strict';

        const coll = db.getMongo().getCollection(options.ns);
        const numOps = options.numOps;
        const id = options.id;

        testLog('Starting to apply ' + numOps + ' operations in collection ' + coll.getFullName());
        applyOpsInsertNonAtomic(coll, numOps, id);
        testLog('Successfully applied ' + numOps + ' operations in collection ' +
                coll.getFullName());
    }

    /**
     * Creates a function for startParallelShell() to run that will insert documents into
     * collection 'coll' using applyOps.
     */
    function createInsertFunction(coll, numOps, id) {
        const options = {
            ns: coll.getFullName(),
            numOps: numOps,
            id: id,
        };
        const functionName = 'insertFunction_' + coll.getFullName().replace(/\./g, '_');
        const s =                                                                     //
            '\n\n' +                                                                  //
            'const testLog = ' + testLog + ';\n\n' +                                  //
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

    /**
     * Runs the test.
     */
    this.run = function() {
        const options = this.options;

        assert(options.ns1, 'collection 1 namespace not provided');
        assert(options.ns2, 'collection 2 namespace not provided');

        const replTest = new ReplSetTest({nodes: 1});
        replTest.startSet();
        replTest.initiate();

        const primary = replTest.getPrimary();
        const adminDb = primary.getDB('admin');

        if (options.requiresDocumentLevelConcurrency &&
            !supportsDocumentLevelConcurrency(adminDb)) {
            testLog('Skipping test because storage engine does not support document level ' +
                    'concurrency.');
            return;
        }

        const coll1 = primary.getCollection(options.ns1);
        const db1 = coll1.getDB();
        const coll2 = primary.getCollection(options.ns2);
        const db2 = coll2.getDB();

        assert.commandWorked(db1.createCollection(coll1.getName()));
        if (coll1.getFullName() !== coll2.getFullName()) {
            assert.commandWorked(db2.createCollection(coll2.getName()));
        }

        // Enable fail point to pause applyOps between operations.
        assert.commandWorked(primary.adminCommand(
            {configureFailPoint: 'applyOpsPauseBetweenOperations', mode: 'alwaysOn'}));

        // This logs each operation being applied.
        const previousLogLevel =
            assert.commandWorked(primary.setLogLevel(3, 'replication')).was.replication.verbosity;

        testLog('Applying operations in collections ' + coll1.getFullName() + ' and ' +
                coll2.getFullName());

        const numOps = 100;
        const insertProcess1 =
            startParallelShell(createInsertFunction(coll1, numOps, 0), replTest.getPort(0));
        const insertProcess2 =
            startParallelShell(createInsertFunction(coll2, numOps, 1), replTest.getPort(0));

        // The fail point will prevent applyOps from advancing past the first operation in each
        // batch of operations. If applyOps is applying both sets of operations concurrently without
        // holding the global lock, the insert opcounter will eventually be incremented to 2.
        try {
            let insertOpCount = 0;
            const expectedFinalOpCount = 2;
            assert.soon(
                function() {
                    const serverStatus = adminDb.serverStatus();
                    insertOpCount = getInsertOpCount(serverStatus);
                    // This assertion may fail if the fail point is not implemented correctly within
                    // applyOps. This allows us to fail fast instead of waiting for the
                    // assert.soon() function to time out.
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

        testLog('Successfully applied operations in collections ' + coll1.getFullName() + ' and ' +
                coll2.getFullName());

        // Reset log level.
        primary.setLogLevel(previousLogLevel, 'replication');

        const serverStatus = adminDb.serverStatus();
        assert.eq(200,
                  getInsertOpCount(serverStatus),
                  'incorrect number of insert operations in server status after applyOps: ' +
                      tojson(serverStatus));

        replTest.stopSet();
    };
};
