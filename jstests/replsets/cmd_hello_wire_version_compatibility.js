// This test is to ensure that we do not increase the minWireversion or decrease the max
// wire version in API Version 1.
// @tags: [
//        requires_fcv_49,
// ]

// See wire_version.h
const RELEASE_2_4_AND_BEFORE = NumberLong(0);
const WIRE_VERSION_49 = NumberLong(12);

const VERSION_4_9_COMPATIBILITY = {
    minWireVersion: RELEASE_2_4_AND_BEFORE,
    maxWireVersion: WIRE_VERSION_49,
};

(function() {
"use strict";

let testWireVersion = function(isSystem, compatibilityBounds) {
    const rst = new ReplSetTest({nodes: 3, auth: ""});
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    let admin = primary.getDB("local");

    if (isSystem) {
        admin.auth("__system", "");
    }

    {  // Test deprecated isMaster command
        let res = admin.runCommand({isMaster: 1, apiVersion: "1", apiStrict: true});
        assert.lte(res.minWireVersion, compatibilityBounds.minWireVersion);
        assert.gte(res.maxWireVersion, compatibilityBounds.maxWireVersion);
    }

    {  // Test new hello command
        let res = admin.runCommand({hello: 1, apiVersion: "1", apiStrict: true});
        assert.lte(res.minWireVersion, compatibilityBounds.minWireVersion);
        assert.gte(res.maxWireVersion, compatibilityBounds.maxWireVersion);
    }

    rst.stopSet();
};

// Test API version 1.
// Version 4.9 sends the same min and max wire version for internal and
// external user.
testWireVersion(false, VERSION_4_9_COMPATIBILITY);
testWireVersion(true, VERSION_4_9_COMPATIBILITY);
})();