function rotate(conn, path, rotateCount) {
    sleep(2000);
    for (let i = 1; i <= rotateCount; ++i) {
        assert.commandWorked(conn.adminCommand({rotateFTDC: true}));
        assert.soon(() => {
            return ls(path).length == i + 1;
        }, "Timeout awaiting file rotate", 30000, 2000);
        jsTestLog("Rotation count " + i + " of " + rotateCount);
    }

    jsTestLog("Found " + ls(path).length + " files in " + path);
}

{
    const singlePath = MongoRunner.toRealPath('ftdclogs-standalone-single-rotate');
    const multiPath = MongoRunner.toRealPath('ftdclogs-standalone-multi-rotate');

    const singleStandalone =
        MongoRunner.runMongod({setParameter: {diagnosticDataCollectionDirectoryPath: singlePath}});
    rotate(singleStandalone, singlePath, 1);

    MongoRunner.stopMongod(singleStandalone);

    const multiStandalone =
        MongoRunner.runMongod({setParameter: {diagnosticDataCollectionDirectoryPath: multiPath}});
    rotate(multiStandalone, multiPath, 25);

    MongoRunner.stopMongod(multiStandalone);
}

{
    const path = MongoRunner.toRealPath('ftdclogs-mongos-multi-rotate');
    mkdir(path);
    const st = new ShardingTest({
        shards: 2,
        mongos: 1,
        mongosOptions: {setParameter: {diagnosticDataCollectionDirectoryPath: path}}
    });

    rotate(st.s, path, 5);

    st.stop();
}
