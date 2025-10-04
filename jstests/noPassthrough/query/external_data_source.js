/**
 * Tests $_externalDataSources aggregate command option.
 *
 * @tags: [
 * # This test file requires multi-threading for writers and tends to fail on small machines due to
 * # thread resource shortage
 * requires_external_data_source,
 * ]
 */
import {aggPlanHasStage} from "jstests/libs/query/analyze_plan.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";

// Runs tests on a standalone mongod.
let conn = MongoRunner.runMongod({setParameter: {enableComputeMode: true}});
let db = conn.getDB(jsTestName());

const kUrlProtocolFile = "file://";
const hostInfo = assert.commandWorked(db.hostInfo());
const kDefaultPipePath = (() => {
    return hostInfo.os.type == "Windows" ? "//./pipe/" : "/tmp/";
})();

function getRandomPipeName() {
    return "external_data_source_" + extractUUIDFromObject(UUID());
}

// Empty option
assert.throwsWithCode(() => {
    db.coll.aggregate([{$match: {a: 1}}], {$_externalDataSources: []});
}, 7039002);

// No external file metadata
assert.throwsWithCode(() => {
    db.coll.aggregate([{$match: {a: 1}}], {$_externalDataSources: [{collName: "coll", dataSources: []}]});
}, 7039001);

// No file type
assert.throwsWithCode(() => {
    db.coll.aggregate([{$match: {a: 1}}], {
        $_externalDataSources: [
            {
                collName: "coll",
                dataSources: [{url: kUrlProtocolFile + getRandomPipeName(), storageType: "pipe"}],
            },
        ],
    });
}, ErrorCodes.IDLFailedToParse);

// Unknown file type
assert.throwsWithCode(() => {
    db.coll.aggregate([{$match: {a: 1}}], {
        $_externalDataSources: [
            {
                collName: "coll",
                dataSources: [
                    {
                        url: kUrlProtocolFile + getRandomPipeName(),
                        storageType: "pipe",
                        fileType: "unknown",
                    },
                ],
            },
        ],
    });
}, ErrorCodes.BadValue);

// No storage type
assert.throwsWithCode(() => {
    db.coll.aggregate([{$match: {a: 1}}], {
        $_externalDataSources: [
            {
                collName: "coll",
                dataSources: [{url: kUrlProtocolFile + getRandomPipeName(), fileType: "bson"}],
            },
        ],
    });
}, ErrorCodes.IDLFailedToParse);

// Unknown storage type
assert.throwsWithCode(() => {
    db.coll.aggregate([{$match: {a: 1}}], {
        $_externalDataSources: [
            {
                collName: "coll",
                dataSources: [
                    {
                        url: kUrlProtocolFile + getRandomPipeName(),
                        storageType: "unknown",
                        fileType: "bson",
                    },
                ],
            },
        ],
    });
}, ErrorCodes.BadValue);

// No url
assert.throwsWithCode(() => {
    db.coll.aggregate([{$match: {a: 1}}], {
        $_externalDataSources: [{collName: "coll", dataSources: [{storageType: "pipe", fileType: "bson"}]}],
    });
}, ErrorCodes.IDLFailedToParse);

// Invalid url #1: Unsupported protocol for 'pipe'
assert.throwsWithCode(() => {
    db.coll.aggregate([{$match: {a: 1}}], {
        $_externalDataSources: [
            {
                collName: "coll",
                dataSources: [{url: "http://abc.com/name1", storageType: "pipe", fileType: "bson"}],
            },
        ],
    });
}, 6968500);

// Invalid url #2: '..' in the url
assert.throwsWithCode(() => {
    db.coll.aggregate([{$match: {a: 1}}], {
        $_externalDataSources: [
            {
                collName: "coll",
                dataSources: [{url: kUrlProtocolFile + "../name1", storageType: "pipe", fileType: "bson"}],
            },
        ],
    });
}, [7001100, 7001101]);

// The source namespace is not an external data source
assert.throwsWithCode(() => {
    db.unknown.aggregate([{$match: {a: 1}}], {
        $_externalDataSources: [
            {
                collName: "coll",
                dataSources: [{url: kUrlProtocolFile + getRandomPipeName(), storageType: "pipe", fileType: "bson"}],
            },
        ],
    });
}, 7039003);

(function testSampleStageOverExternalDataSourceNotOptimized() {
    const explain = db.coll.explain().aggregate([{$sample: {size: 10}}], {
        $_externalDataSources: [
            {
                collName: "coll",
                dataSources: [{url: kUrlProtocolFile + getRandomPipeName(), storageType: "pipe", fileType: "bson"}],
            },
        ],
    });
    assert(
        aggPlanHasStage(explain, "$sample"),
        `Expected $sample is not optimized into $sampleFromRandomCursor but got ${tojson(explain)}`,
    );
})();

// Verifies that an external data source cannot be used for the $merge / $out pipeline stages.
(function testMergeOrOutStageToExternalDataSource() {
    [{$out: "out"}, {$merge: "out"}].forEach((stage) => {
        assert.throwsWithCode(() => {
            db.coll.aggregate([stage], {
                $_externalDataSources: [
                    {
                        collName: "coll",
                        dataSources: [
                            {
                                url: kUrlProtocolFile + getRandomPipeName(),
                                storageType: "pipe",
                                fileType: "bson",
                            },
                        ],
                    },
                    {
                        collName: "out",
                        dataSources: [
                            {
                                url: kUrlProtocolFile + getRandomPipeName(),
                                storageType: "pipe",
                                fileType: "bson",
                            },
                        ],
                    },
                ],
            });
        }, 7239302);
    });
})();

(function testCollectionlessAgg() {
    const docs = [{a: 1}, {a: 2}, {a: 3}];
    assert.sameMembers(docs, db.aggregate([{$documents: docs}]).toArray());
})();

(function testCollectionlessAggWithExternalDataSources() {
    assert.throwsWithCode(() => {
        db.aggregate([{$documents: [{a: 1}]}], {$_externalDataSources: []});
    }, 7604400);
})();

//
// Named Pipes success test cases follow.
//

////////////////////////////////////////////////////////////////////////////////////////////////////
// Test production code for MultiBsonStreamCursor and below, plus the shell pipe writer and reader
// functions, separately from $_externalDataSources to help narrow down the source of any failures.
////////////////////////////////////////////////////////////////////////////////////////////////////
let objsPerPipe = 25;
(function testBasicPipeReadWrite() {
    jsTestLog("Testing testBasicPipeReadWrite()");

    const pipeName1 = getRandomPipeName();
    const pipeName2 = pipeName1 + "1";
    _writeTestPipe(pipeName1, objsPerPipe);
    _writeTestPipe(pipeName2, objsPerPipe);
    let result = _readTestPipes(pipeName1, pipeName2);
    assert.eq(
        2 * objsPerPipe,
        bsonObjToArray(result)[0], // "objects" first field contains the count of objects read
        "_readTestPipes read wrong number of objects: " + bsonObjToArray(result)[0],
    );
})();

function testSimpleAggregationsOverExternalDataSource(pipeDir) {
    ////////////////////////////////////////////////////////////////////////////////////////////////
    // Test that $_externalDataSource can read and aggregate multiple named pipes.
    ////////////////////////////////////////////////////////////////////////////////////////////////
    (function testCountOverMultiplePipes() {
        jsTestLog("Testing testCountOverMultiplePipes()");

        objsPerPipe = 100;
        const pipeName1 = getRandomPipeName();
        const pipeName2 = pipeName1 + "1";
        _writeTestPipe(pipeName1, objsPerPipe, 0, 2048, pipeDir);
        _writeTestPipe(pipeName2, objsPerPipe, 0, 2048, pipeDir);
        let result = db.coll.aggregate([{$count: "objects"}], {
            $_externalDataSources: [
                {
                    collName: "coll",
                    dataSources: [
                        {url: kUrlProtocolFile + pipeName1, storageType: "pipe", fileType: "bson"},
                        {url: kUrlProtocolFile + pipeName2, storageType: "pipe", fileType: "bson"},
                    ],
                },
            ],
        });
        assert.eq(
            2 * objsPerPipe,
            result._batch[0].objects, // shell puts agg result in "_batch"[0] field of a wrapper obj
            "$_externalDataSources read wrong number of objects: " + result._batch[0].objects,
        );
    })();

    ////////////////////////////////////////////////////////////////////////////////////////////////
    // Test correctness by verifying reading from the pipes returns the same objects written to
    // them.
    ////////////////////////////////////////////////////////////////////////////////////////////////
    // The following objects are also in BSON file external_data_source.bson in the same order.
    const kPipes = 2; // number of pipes to write
    const kObjsToWrite = [
        {"Zero": "zero zero zero zero zero zero zero zero zero zero zero zero zero zero zero zero"},
        {"One": "one one one one one one one one one one one one one one one one one one one one"},
        {"Two": "two two two two two two two two two two two two two two two two two two two two"},
        {"Three": "three three three three three three three three three three three three three"},
        {"Four": "four four four four four four four four four four four four four four four four"},
        {"Five": "five five five five five five five five five five five five five five five five"},
        {"Six": "six six six six six six six six six six six six six six six six six six six six"},
    ];
    const kNumObjs = kObjsToWrite.length; // number of different objects for round-robin
    (function testRoundtripOverMultiplePipes() {
        jsTestLog("Testing testRoundtripOverMultiplePipes()");

        const pipeName1 = getRandomPipeName();
        const pipeName2 = pipeName1 + "1";
        _writeTestPipeObjects(pipeName1, objsPerPipe, kObjsToWrite, pipeDir);
        _writeTestPipeObjects(pipeName2, objsPerPipe, kObjsToWrite, pipeDir);
        let cursor = db.coll.aggregate([], {
            $_externalDataSources: [
                {
                    collName: "coll",
                    dataSources: [
                        {url: kUrlProtocolFile + pipeName1, storageType: "pipe", fileType: "bson"},
                        {url: kUrlProtocolFile + pipeName2, storageType: "pipe", fileType: "bson"},
                    ],
                },
            ],
        });
        // Verify the objects read from the pipes match what was written to them.
        for (let pipe = 0; pipe < kPipes; ++pipe) {
            for (let objIdx = 0; objIdx < objsPerPipe; ++objIdx) {
                assert.eq(
                    cursor.next(),
                    kObjsToWrite[objIdx % kNumObjs],
                    "Object read from pipe does not match expected.",
                );
            }
        }
    })();

    ////////////////////////////////////////////////////////////////////////////////////////////////
    // Test _writeTestPipeBsonFile() correctness by verifying it writes objects from
    // external_data_source.bson correctly as read back via $_externalDataSources. This is the same
    // as prior test except for using _writeTestPipeBsonFile() instead of _writeTestPipeObjects().
    ////////////////////////////////////////////////////////////////////////////////////////////////
    (function testRoundtripOverMultiplePipesUsingBsonFile() {
        jsTestLog("Testing testRoundtripOverMultiplePipesUsingBsonFile()");

        const pipeName1 = getRandomPipeName();
        const pipeName2 = pipeName1 + "1";
        _writeTestPipeBsonFile(
            pipeName1,
            objsPerPipe,
            "jstests/noPassthrough/query/external_data_source.bson",
            pipeDir,
        );
        _writeTestPipeBsonFile(
            pipeName2,
            objsPerPipe,
            "jstests/noPassthrough/query/external_data_source.bson",
            pipeDir,
        );
        let cursor = db.coll.aggregate([{$project: {_id: 0}}], {
            $_externalDataSources: [
                {
                    collName: "coll",
                    dataSources: [
                        {url: kUrlProtocolFile + pipeName1, storageType: "pipe", fileType: "bson"},
                        {url: kUrlProtocolFile + pipeName2, storageType: "pipe", fileType: "bson"},
                    ],
                },
            ],
        });
        // Verify the objects read from the pipes match what was written to them.
        for (let pipe = 0; pipe < kPipes; ++pipe) {
            for (let objIdx = 0; objIdx < objsPerPipe; ++objIdx) {
                assert.eq(
                    cursor.next(),
                    kObjsToWrite[objIdx % kNumObjs],
                    "Object read from pipe does not match expected.",
                );
            }
        }
    })();

    // Prepares data for $match / $group / $unionWith / spill test cases.
    jsTestLog("Preparing data for $match / $group / $unionWith/ spill");
    Random.setRandomSeed();
    const collObjs = [];
    const kNumGroups = 10;
    const kDocs = 1000;
    for (let i = 0; i < kDocs; ++i) {
        collObjs.push({
            _id: i,
            g: Random.randInt(kNumGroups), // 10 groups
            str1: "strdata_" + Random.randInt(100000000),
        });
    }

    (function testMatchOverExternalDataSource() {
        jsTestLog("Testing testMatchOverExternalDataSource()");

        const pipeName1 = getRandomPipeName();
        _writeTestPipeObjects(pipeName1, collObjs.length, collObjs, pipeDir);

        const kNumFilter = 5;
        const expectedRes = collObjs.filter((obj) => obj.g < kNumFilter);
        const cursor = db.coll.aggregate([{$match: {g: {$lt: kNumFilter}}}], {
            $_externalDataSources: [
                {
                    collName: "coll",
                    dataSources: [{url: kUrlProtocolFile + pipeName1, storageType: "pipe", fileType: "bson"}],
                },
            ],
        });
        const resArr = cursor.toArray();
        assert.eq(resArr.length, expectedRes.length);
        for (let i = 0; i < expectedRes.length; ++i) {
            assert.eq(resArr[i], expectedRes[i], `Expected ${tojson(expectedRes[i])} but got ${tojson(resArr[i])}`);
        }
    })();

    // Computes {$group: {_id: "$g", c: {$count: {}}} manually.
    function getCountPerGroupResult(collObjs) {
        const countPerGroup = [];
        for (let i = 0; i < kNumGroups; ++i) {
            countPerGroup[i] = 0;
        }
        collObjs.forEach((obj) => {
            ++countPerGroup[obj.g];
        });
        const expectedRes = [];
        countPerGroup.forEach((cnt, idx) => {
            if (cnt > 0) {
                expectedRes.push({_id: idx, c: cnt});
            }
        });

        return expectedRes;
    }

    (function testGroupOverExternalDataSource() {
        jsTestLog("Testing testGroupOverExternalDataSource()");

        const pipeName1 = getRandomPipeName();
        _writeTestPipeObjects(pipeName1, collObjs.length, collObjs, pipeDir);

        const expectedRes = getCountPerGroupResult(collObjs);
        const cursor = db.coll.aggregate([{$group: {_id: "$g", c: {$count: {}}}}], {
            $_externalDataSources: [
                {
                    collName: "coll",
                    dataSources: [{url: kUrlProtocolFile + pipeName1, storageType: "pipe", fileType: "bson"}],
                },
            ],
        });
        const resArr = cursor.toArray();
        assert.sameMembers(resArr, expectedRes);
    })();

    (function testUnionWithOverExternalDataSource() {
        jsTestLog("Testing testUnionWithOverExternalDataSource()");

        const pipeName1 = getRandomPipeName();
        const pipeName2 = getRandomPipeName() + "1";
        _writeTestPipeObjects(pipeName1, collObjs.length, collObjs, pipeDir);
        _writeTestPipeObjects(pipeName2, collObjs.length, collObjs, pipeDir);

        const expectedRes = collObjs.concat(collObjs);
        const collName1 = "coll1";
        const collName2 = "coll2";
        const cursor = db[collName1].aggregate([{$unionWith: collName2}], {
            $_externalDataSources: [
                {
                    collName: collName1,
                    dataSources: [{url: kUrlProtocolFile + pipeName1, storageType: "pipe", fileType: "bson"}],
                },
                {
                    collName: collName2,
                    dataSources: [{url: kUrlProtocolFile + pipeName2, storageType: "pipe", fileType: "bson"}],
                },
            ],
        });
        const resArr = cursor.toArray();
        assert.eq(resArr.length, expectedRes.length);
        for (let i = 0; i < expectedRes.length; ++i) {
            assert.eq(resArr[i], expectedRes[i], `Expected ${tojson(expectedRes[i])} but got ${tojson(resArr[i])}`);
        }
    })();

    (function testSpillingGroupOverExternalDataSource() {
        jsTestLog("Testing testSpillingGroupOverExternalDataSource()");

        // Makes sure that both classic/SBE $group spill data.
        const oldClassicGroupMaxMemory = assert.commandWorked(
            db.adminCommand({
                setParameter: 1,
                internalDocumentSourceGroupMaxMemoryBytes: 1,
            }),
        ).was;
        const oldSbeGroupMaxMemory = assert.commandWorked(
            db.adminCommand({
                setParameter: 1,
                internalQuerySlotBasedExecutionHashAggApproxMemoryUseInBytesBeforeSpill: 1,
            }),
        ).was;

        const pipeName1 = getRandomPipeName();
        _writeTestPipeObjects(pipeName1, collObjs.length, collObjs, pipeDir);

        const expectedRes = getCountPerGroupResult(collObjs);
        const cursor = db.coll.aggregate([{$group: {_id: "$g", c: {$count: {}}}}], {
            $_externalDataSources: [
                {
                    collName: "coll",
                    dataSources: [{url: kUrlProtocolFile + pipeName1, storageType: "pipe", fileType: "bson"}],
                },
            ],
        });
        const resArr = cursor.toArray();
        assert.sameMembers(resArr, expectedRes);

        assert.commandWorked(
            db.adminCommand({
                setParameter: 1,
                internalDocumentSourceGroupMaxMemoryBytes: oldClassicGroupMaxMemory,
            }),
        );
        assert.commandWorked(
            db.adminCommand({
                setParameter: 1,
                internalQuerySlotBasedExecutionHashAggApproxMemoryUseInBytesBeforeSpill: oldSbeGroupMaxMemory,
            }),
        );
    })();

    // Verifies that 'killCursors' command works over external data sources while the server keeps
    // reading data from a named pipe. Reading external data sources should be interruptible.
    (function testKillCursorOverExternalDataSource() {
        jsTestLog("Testing testKillCursorOverExternalDataSource()");

        // Prepares a large dataset.
        const largeCollObjs = [];
        const kManyDocs = 250000;
        for (let i = 0; i < kManyDocs; ++i) {
            largeCollObjs.push({
                _id: i,
                g: Random.randInt(10), // 10 groups
                str1: "strdata_" + Random.randInt(100000000),
            });
        }

        // We read 2 collections using $unionWith so that the result set cannot be fit into one
        // result batch for the 'getMore' request.
        const collName1 = "coll1";
        const collName2 = "coll2";

        // 250K docs almost reaches 16MB BSONObj size limit.
        const pipeName1 = getRandomPipeName();
        const pipeName2 = getRandomPipeName() + "1";
        _writeTestPipeObjects(pipeName1, largeCollObjs.length, largeCollObjs, pipeDir);
        _writeTestPipeObjects(pipeName2, largeCollObjs.length, largeCollObjs, pipeDir);

        let cursor = db[collName1].aggregate([{$unionWith: collName2}], {
            $_externalDataSources: [
                {
                    collName: collName1,
                    dataSources: [{url: kUrlProtocolFile + pipeName1, storageType: "pipe", fileType: "bson"}],
                },
                {
                    collName: collName2,
                    dataSources: [{url: kUrlProtocolFile + pipeName2, storageType: "pipe", fileType: "bson"}],
                },
            ],
        });

        // Has 'getMore' command issued to the server since the default batch size is 101 documents.
        for (let i = 0; i < 102; ++i) {
            cursor.next();
        }

        // Has 'killCursors' command issued to the server while the server is reading more data from
        // named pipes to send the next batch. If this fails, an exception will be thrown.
        cursor.close();
    })();
}

jsTestLog("Testing successful named pipe test cases");
testSimpleAggregationsOverExternalDataSource(kDefaultPipePath);

MongoRunner.stopMongod(conn);

// The 'externalPipeDir' is effective only on POSIX-like system.
if (hostInfo.os.type != "Windows") {
    // Verfies that 'externalPipeDir' server parameter works with the same test cases.
    (function testExternalPipeDirWorks() {
        jsTestLog("Testing testExternalPipeDirWorks()");

        const pipeDir = MongoRunner.dataDir + "/tmp/";
        assert(mkdir(pipeDir).created, `Failed to create ${pipeDir}`);

        jsTestLog(`Testing named pipe test cases with externalPipeDir=${pipeDir}`);
        conn = MongoRunner.runMongod({setParameter: {enableComputeMode: true, externalPipeDir: pipeDir}});
        db = conn.getDB(jsTestName());

        testSimpleAggregationsOverExternalDataSource(pipeDir);

        MongoRunner.stopMongod(conn);
    })();

    // Verifies that 'externalPipeDir' with '..' is rejected.
    (function testInvalidExternalPipeDirRejected() {
        jsTestLog("Testing testInvalidExternalPipeDirRejected()");

        const pipeDir = MongoRunner.dataDir + "/tmp/abc/../def/";
        assert(mkdir(pipeDir).created, `Failed to create ${pipeDir}`);

        jsTestLog(`Testing externalPipeDir=${pipeDir}`);
        const pid = MongoRunner.runMongod({
            waitForConnect: false,
            setParameter: {enableComputeMode: true, externalPipeDir: pipeDir},
        }).pid;
        assert.soon(
            () => {
                const runningStatus = checkProgram(pid);
                return !runningStatus.alive && runningStatus.exitCode != 0;
            },
            "Expected mongod died due to an error",
            120 * 1000,
        );
    })();
}
