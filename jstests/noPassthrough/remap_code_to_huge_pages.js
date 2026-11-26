/**
 * Tests that a mongod started with 'remapCodeToHugePages=true' remaps its code at startup and then
 * serves requests normally, and that nothing is remapped with the default value of the parameter.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 * ]
 */

import {isLinux} from "jstests/libs/server_security/os_helpers.js";

if (!isLinux()) {
    jsTest.log("Skipping test since 'remapCodeToHugePages' is only available on Linux.");
    quit();
}

const kRemappedLogId = 11520101;
const kRemapFailedLogId = 11520100;

function transparentHugePagesEnabled() {
    try {
        return !cat("/sys/kernel/mm/transparent_hugepage/enabled").includes("[never]");
    } catch (e) {
        return false;
    }
}

function runWorkload(conn) {
    const coll = conn.getDB("test").remap_code_to_huge_pages;
    const docs = [];
    for (let i = 0; i < 1000; ++i) {
        docs.push({_id: i, x: i % 10, s: "a".repeat(i % 100)});
    }
    assert.commandWorked(coll.insertMany(docs));
    assert.commandWorked(coll.createIndex({x: 1}));
    assert.eq(100, coll.find({x: 3}).itcount());
    assert.eq(10, coll.aggregate([{$group: {_id: "$x", n: {$sum: 1}}}]).itcount());
    assert.commandWorked(coll.updateMany({x: 3}, {$inc: {x: 100}}));
    assert.eq(100, coll.countDocuments({x: 103}));
    assert.commandWorked(coll.validate({full: true}));
}

(function remapEnabled() {
    const conn = MongoRunner.runMongod({setParameter: {remapCodeToHugePages: true}});
    assert.neq(null, conn, "mongod was unable to start up with remapCodeToHugePages=true");

    const res = assert.commandWorked(conn.adminCommand({getParameter: 1, remapCodeToHugePages: 1}));
    assert.eq(true, res.remapCodeToHugePages, res);

    if (transparentHugePagesEnabled()) {
        // The remap ran. Builds whose code segments are not laid out for 2MB pages remap nothing.
        checkLog.containsJson(conn, kRemappedLogId);
        assert(
            !checkLog.checkContainsOnceJson(conn, kRemapFailedLogId, {}),
            "a code range failed to remap",
        );
    } else {
        jsTest.log("Transparent huge pages are disabled, nothing is remapped.");
        assert(!checkLog.checkContainsOnceJson(conn, kRemappedLogId, {}));
    }

    runWorkload(conn);
    assert.eq(0, MongoRunner.stopMongod(conn));
})();

(function remapDisabledByDefault() {
    const conn = MongoRunner.runMongod();
    assert.neq(null, conn, "mongod was unable to start up");

    const res = assert.commandWorked(conn.adminCommand({getParameter: 1, remapCodeToHugePages: 1}));
    assert.eq(false, res.remapCodeToHugePages, res);
    assert(!checkLog.checkContainsOnceJson(conn, kRemappedLogId, {}));

    runWorkload(conn);
    assert.eq(0, MongoRunner.stopMongod(conn));
})();
