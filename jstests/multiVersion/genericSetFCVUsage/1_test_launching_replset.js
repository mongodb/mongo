//
// Tests launching multi-version ReplSetTest replica sets
//
//

load('jstests/multiVersion/libs/verify_versions.js');

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

if (MongoRunner.areBinVersionsTheSame("last-continuous", "last-lts")) {
    jsTest.log("Skipping test because 'last-continuous' == 'last-lts'");
    return;
}

for (let versions of [["last-lts", "last-continuous"], ["last-continuous", "last-lts"]]) {
    jsTestLog("Testing mixed versions: " + tojson(versions));

    var rst = new ReplSetTest({nodes: 2});
    rst.startSet({binVersion: versions});
    let err = assert.throws(() => rst.initiate());
    assert(err.message.includes(
               "Can only specify one of 'last-lts' and 'last-continuous' in binVersion, not both."),
           err);
    rst.stopSet();
}

for (let versions of [["last-lts", "last-continuous"], ["last-continuous", "last-lts"]]) {
    jsTestLog("Testing mixed versions: " + tojson(versions));

    const rst = new ReplSetTest({
        nodes: [
            {
                binVersion: versions[0],
            },
            {
                binVersion: versions[1],
            },
        ]
    });
    rst.startSet();

    let err = assert.throws(() => rst.initiate());
    assert(err.message.includes(
               "Can only specify one of 'last-lts' and 'last-continuous' in binVersion, not both."),
           err);
    rst.stopSet();
}

jsTestLog("Done!");
})();

//
// End
//
