/*
 * Tests that ReplSetTest#checkReplicatedDataHashes() retries a transient network error from the
 * connectionStatus command that asCluster() runs, rather than failing the check.
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

describe("checkReplicatedDataHashes connectionStatus retry", function () {
    before(function () {
        this.rst = new ReplSetTest({name: jsTestName(), nodes: 2});
        this.rst.startSet();
        this.rst.initiate();
    });

    after(function () {
        this.rst.stopSet();
    });

    it("retries a network error from connectionStatus", function () {
        const primary = this.rst.getPrimary();
        const originalAdminCommand = primary.adminCommand;
        let failuresLeft = 1;
        primary.adminCommand = function (cmd, ...rest) {
            if (typeof cmd === "object" && cmd.connectionStatus && failuresLeft > 0) {
                failuresLeft--;
                throw new Error("network error while attempting to run command 'connectionStatus'");
            }
            return originalAdminCommand.call(this, cmd, ...rest);
        };
        try {
            this.rst.checkReplicatedDataHashes();
        } finally {
            primary.adminCommand = originalAdminCommand;
        }
        assert.eq(failuresLeft, 0, "expected the injected connectionStatus failure to be hit");
    });
});
