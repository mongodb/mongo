/**
 * Checks if authentication commands are marked as non-deprioritizable
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {getTotalMarkedNonDeprioritizableCount} from "jstests/noPassthrough/admission/execution_control/libs/execution_control_helper.js";

describe("Authentication commands are non-deprioritizable", function () {
    before(() => {
        this.login = (userObj) => {
            this.rst.getPrimary().getDB(userObj.db).auth(userObj.username, userObj.password);
        };

        this.testUser = {db: "test", username: "bar", password: "baz"};

        this.rst = new ReplSetTest({nodes: 1});
        this.rst.startSet();
        this.rst.initiate();

        this.rst
            .getPrimary()
            .getDB(this.testUser.db)
            .createUser({user: this.testUser.username, pwd: this.testUser.password, roles: jsTest.basicUserRoles});
    });

    after(() => {
        this.rst.stopSet();
    });

    it("Login command is marked as non-deprioritizable", () => {
        const initialNonDeprioritizableCount = getTotalMarkedNonDeprioritizableCount(this.rst.getPrimary());

        this.login(this.testUser);

        assert.soon(() => {
            const nonDeprioCount = getTotalMarkedNonDeprioritizableCount(this.rst.getPrimary());
            return nonDeprioCount.toNumber() === initialNonDeprioritizableCount.toNumber() + 1;
        });
    });
});
