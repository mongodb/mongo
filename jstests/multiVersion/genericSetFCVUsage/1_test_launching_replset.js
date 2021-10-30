//
// Tests launching multi-version ReplSetTest replica sets
//
//

load('./jstests/multiVersion/libs/verify_versions.js');

(function() {
"use strict";

for (let version of ["last-lts", "last-continuous", "latest"]) {
    jsTestLog("Testing single version: " + version);

    // Set up a single-version replica set
    var rst = new ReplSetTest({nodes: 2});

    rst.startSet({binVersion: version});

    var nodes = rst.nodes;

    // Make sure the started versions are actually the correct versions
    for (var j = 0; j < nodes.length; j++)
        assert.binVersion(nodes[j], version);

    rst.stopSet();
}

for (let versions of [["last-lts", "latest"], ["last-continuous", "latest"]]) {
    jsTestLog("Testing mixed versions: " + tojson(versions));

    // Set up a multi-version replica set
    var rst = new ReplSetTest({nodes: 2});

    rst.startSet({binVersion: versions});

    var nodes = rst.nodes;

    // Make sure we have hosts of all the different versions
    var versionsFound = [];
    for (var j = 0; j < nodes.length; j++)
        versionsFound.push(nodes[j].getBinVersion());

    assert.allBinVersions(versions, versionsFound);

    rst.stopSet();
}

// TODO(SERVER-61100): Re-enable this test.
if (true) {
    jsTestLog("Skipping test as it is currently disabled.");
    return;
}

for (let versions of [["last-lts", "last-continuous"], ["last-continuous", "last-lts"]]) {
    jsTestLog("Testing mixed versions: " + tojson(versions));

    try {
        var rst = new ReplSetTest({nodes: 2});
        rst.startSet({binVersion: versions});
        rst.initiate();
    } catch (e) {
        if (e instanceof Error) {
            if (e.message.includes(
                    "Can only specify one of 'last-lts' and 'last-continuous' in binVersion, not both.")) {
                continue;
            }
        }
        throw e;
    }
    assert(
        MongoRunner.areBinVersionsTheSame("last-continuous", "last-lts"),
        "Should have thrown error in creating ReplSetTest because can only specify one of 'last-lts' and 'last-continuous' in binVersion, not both.");

    var nodes = rst.nodes;

    // Make sure we have hosts of all the different versions
    var versionsFound = [];
    for (var j = 0; j < nodes.length; j++)
        versionsFound.push(nodes[j].getBinVersion());

    assert.allBinVersions(versions, versionsFound);

    rst.stopSet();
}

jsTestLog("Done!");
})();

//
// End
//
