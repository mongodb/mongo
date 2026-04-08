/**
 * Pins the FCV to lastContinuousFCV so that FCV-gated feature flags with a version above
 * lastContinuousFCV are automatically disabled.
 *
 * # TODO (SERVER-109519): Remove this file.
 *
 * Works in two contexts:
 *  - Passthrough suites (shell connected to a running fixture): downgrades FCV immediately.
 *  - nodb suites (tests create their own ShardingTest or ReplSetTest): overrides ShardingTest
 *    and ReplSetTest to downgrade FCV after cluster setup.
 */
import {kOverrideConstructor as kOverrideConstructorForST, ShardingTest} from "jstests/libs/shardingtest.js";
import {kOverrideConstructor as kOverrideConstructorForRST, ReplSetTest} from "jstests/libs/replsettest.js";

if (typeof db !== "undefined") {
    assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: lastContinuousFCV, confirm: true}));
}

ShardingTest[kOverrideConstructorForST] = class ShardingTestWithPinnedFCV extends ShardingTest {
    constructor(params) {
        super(params);
        assert.commandWorked(this.s.adminCommand({setFeatureCompatibilityVersion: lastContinuousFCV, confirm: true}));
    }
};

ReplSetTest[kOverrideConstructorForRST] = class ReplSetTestWithPinnedFCV extends ReplSetTest {
    initiate(...args) {
        super.initiate(...args);
        assert.commandWorked(
            this.getPrimary().adminCommand({setFeatureCompatibilityVersion: lastContinuousFCV, confirm: true}),
        );
    }
};
