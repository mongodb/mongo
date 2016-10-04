
(function() {
    'use strict';

    // 3.2.1 is the final version to use the old style replSetUpdatePosition command.
    var oldVersion = "3.2.1";
    var newVersion = "latest";
    var nodes = {
        n1: {binVersion: oldVersion},
        n2: {binVersion: newVersion},
        n3: {binVersion: oldVersion},
        n4: {binVersion: newVersion},
        n5: {binVersion: oldVersion}
    };
    var host = getHostName();
    var name = 'tags';

    var replTest = new ReplSetTest({name: name, nodes: nodes, useBridge: true});
    var nodes = replTest.nodeList();
    var conns = replTest.startSet();
    var port = replTest.ports;
    replTest.initiate({
        _id: name,
        members: [
            {
              _id: 0,
              host: nodes[0],
              tags: {
                  server: '0',
                  dc: 'ny',
                  ny: '1',
                  rack: 'ny.rk1',
              },
            },
            {
              _id: 1,
              host: nodes[1],
              priority: 2,
              tags: {
                  server: '1',
                  dc: 'ny',
                  ny: '2',
                  rack: 'ny.rk1',
              },
            },
            {
              _id: 2,
              host: nodes[2],
              priority: 3,
              tags: {
                  server: '2',
                  dc: 'ny',
                  ny: '3',
                  rack: 'ny.rk2', 2: 'this',
              },
            },
            {
              _id: 3,
              host: nodes[3],
              tags: {
                  server: '3',
                  dc: 'sf',
                  sf: '1',
                  rack: 'sf.rk1',
              },
            },
            {
              _id: 4,
              host: nodes[4],
              tags: {
                  server: '4',
                  dc: 'sf',
                  sf: '2',
                  rack: 'sf.rk2',
              },
            },
        ],
        settings: {
            getLastErrorModes: {
                '2 dc and 3 server': {
                    dc: 2,
                    server: 3,
                },
                '1 and 2': {
                    2: 1,
                    server: 1,
                },
                '2': {
                    2: 1,
                },
                '3 and 4': {
                    sf: 2,
                },
                '3 or 4': {
                    sf: 1,
                },
            },
        },
    });

    replTest.waitForState(replTest.nodes[2], ReplSetTest.State.PRIMARY, 60 * 1000);
    replTest.awaitReplication();

    // Create collection to guard against timeouts due to file allocation.
    assert.commandWorked(replTest.getPrimary().getDB('foo').createCollection('bar'));
    replTest.awaitReplication();

    var ensurePrimary = function(nodeId, expectedWritableNodes) {
        jsTestLog('Node ' + nodeId + ' (' + replTest.nodes[nodeId].host + ') should be primary.');
        replTest.waitForState(replTest.nodes[nodeId], ReplSetTest.State.PRIMARY, 60 * 1000);
        primary = replTest.getPrimary();
        primary.forceWriteMode('commands');
        var writeConcern = {writeConcern: {w: expectedWritableNodes, wtimeout: 30 * 1000}};
        assert.writeOK(primary.getDB('foo').bar.insert({x: 100}, writeConcern));
        return primary;
    };

    // 2 should eventually stage a priority takeover from the primary.
    var primary = ensurePrimary(2, 3);

    jsTestLog('primary is now 2');
    var config = assert.commandWorked(primary.adminCommand({replSetGetConfig: 1})).config;
    jsTestLog('test configuration = ' + tojson(config));

    jsTestLog('Setting up partitions: [0-1-2] [3] [4]');
    conns[0].disconnect(conns[3]);
    conns[0].disconnect(conns[4]);
    conns[1].disconnect(conns[3]);
    conns[1].disconnect(conns[4]);
    conns[2].disconnect(conns[3]);
    conns[2].disconnect(conns[4]);
    conns[3].disconnect(conns[4]);
    jsTestLog('Done setting up partitions');

    jsTestLog('partitions: nodes with each set of brackets [N1, N2, N3] form a complete network.');
    jsTestLog('partitions: [0-1-2] [3] [4] (only nodes 0 and 1 can replicate from primary node 2');

    var doc = {x: 1};

    // This timeout should be shorter in duration than the server parameter maxSyncSourceLagSecs.
    // Some writes are expected to block for this 'timeout' duration before failing.
    // Depending on the order of heartbeats (containing last committed op time) received
    // by a node, it might hang up on its sync source. This may cause some of the write concern
    // tests to fail.
    var timeout = 20 * 1000;

    jsTestLog('test1');
    primary = ensurePrimary(2, 3);

    jsTestLog('Non-existent write concern should be rejected.');
    options = {writeConcern: {w: 'blahblah', wtimeout: timeout}};
    assert.writeOK(primary.getDB('foo').bar.insert(doc));
    var result = assert.writeError(primary.getDB('foo').bar.insert(doc, options));
    assert.neq(null, result.getWriteConcernError());
    assert.eq(ErrorCodes.UnknownReplWriteConcern,
              result.getWriteConcernError().code,
              tojson(result.getWriteConcernError()));

    jsTestLog('Write concern "3 or 4" should fail - 3 and 4 are not connected to the primary.');
    var options = {writeConcern: {w: '3 or 4', wtimeout: timeout}};
    assert.writeOK(primary.getDB('foo').bar.insert(doc));
    result = primary.getDB('foo').bar.insert(doc, options);
    assert.neq(null, result.getWriteConcernError());
    assert(result.getWriteConcernError().errInfo.wtimeout);

    conns[1].reconnect(conns[4]);
    jsTestLog('partitions: [0-1-2] [1-4] [3] ' +
              '(all nodes besides node 3 can replicate from primary node 2)');
    primary = ensurePrimary(2, 4);

    jsTestLog('Write concern "3 or 4" should work - 4 is now connected to the primary ' +
              primary.host + ' via node 1 ' + replTest.nodes[1].host);
    options = {writeConcern: {w: '3 or 4', wtimeout: timeout}};
    assert.writeOK(primary.getDB('foo').bar.insert(doc));
    assert.writeOK(primary.getDB('foo').bar.insert(doc, options));

    jsTestLog('Write concern "3 and 4" should fail - 3 is not connected to the primary.');
    options = {writeConcern: {w: '3 and 4', wtimeout: timeout}};
    assert.writeOK(primary.getDB('foo').bar.insert(doc));
    result = assert.writeError(primary.getDB('foo').bar.insert(doc, options));
    assert.neq(null, result.getWriteConcernError());
    assert(result.getWriteConcernError().errInfo.wtimeout, tojson(result.getWriteConcernError()));

    conns[3].reconnect(conns[4]);
    jsTestLog('partitions: [0-1-2] [1-4] [3-4] ' +
              '(all secondaries can replicate from primary node 2)');
    primary = ensurePrimary(2, 5);

    jsTestLog('31004 should sync from 31001 (31026)');
    jsTestLog('31003 should sync from 31004 (31024)');
    jsTestLog('Write concern "3 and 4" should work - ' +
              'nodes 3 and 4 are connected to primary via node 1.');
    options = {writeConcern: {w: '3 and 4', wtimeout: timeout}};
    assert.writeOK(primary.getDB('foo').bar.insert(doc));
    assert.writeOK(primary.getDB('foo').bar.insert(doc, options));

    jsTestLog('Write concern "2" - writes to primary only.');
    options = {writeConcern: {w: '2', wtimeout: 0}};
    assert.writeOK(primary.getDB('foo').bar.insert(doc));
    assert.writeOK(primary.getDB('foo').bar.insert(doc, options));

    jsTestLog('Write concern "1 and 2"');
    options = {writeConcern: {w: '1 and 2', wtimeout: 0}};
    assert.writeOK(primary.getDB('foo').bar.insert(doc));
    assert.writeOK(primary.getDB('foo').bar.insert(doc, options));

    jsTestLog('Write concern "2 dc and 3 server"');
    primary = ensurePrimary(2, 5);
    options = {writeConcern: {w: '2 dc and 3 server', wtimeout: timeout}};
    assert.writeOK(primary.getDB('foo').bar.insert(doc));
    assert.writeOK(primary.getDB('foo').bar.insert(doc, options));

    jsTestLog('Bringing down current primary node 2 ' + primary.host +
              ' to allow next higher priority node 1 ' + replTest.nodes[1].host +
              ' to become primary.');

    // Is this necessary since 3 will be connected to the new primary via node 4?
    conns[1].reconnect(conns[3]);

    conns[2].disconnect(conns[0]);
    conns[2].disconnect(conns[1]);

    // Is this necessary when we partition node 2 off from the rest of the nodes?
    replTest.stop(2);
    jsTestLog('partitions: [0-1] [2] [1-3-4] ' +
              '(all secondaries except down node 2 can replicate from new primary node 1)');

    // Node 1 with slightly higher priority will take over.
    jsTestLog('1 must become primary here because otherwise the other members will take too ' +
              'long timing out their old sync threads');
    primary = ensurePrimary(1, 4);

    jsTestLog('Write concern "3 and 4" should still work with new primary node 1 ' + primary.host);
    options = {writeConcern: {w: '3 and 4', wtimeout: timeout}};
    assert.writeOK(primary.getDB('foo').bar.insert(doc));
    assert.writeOK(primary.getDB('foo').bar.insert(doc, options));

    jsTestLog('Write concern "2" should fail because node 2 ' + replTest.nodes[2].host +
              ' is down.');
    options = {writeConcern: {w: '2', wtimeout: timeout}};
    assert.writeOK(primary.getDB('foo').bar.insert(doc));
    result = assert.writeError(primary.getDB('foo').bar.insert(doc, options));
    assert.neq(null, result.getWriteConcernError());
    assert(result.getWriteConcernError().errInfo.wtimeout);

    replTest.stopSet();
    jsTestLog('tags.js SUCCESS');
}());
