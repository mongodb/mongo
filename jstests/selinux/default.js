// This test does not run any code. As long as mongod is
// up and running, it is successful

import {SelinuxBaseTest} from "jstests/selinux/lib/selinux_base_test.js";

export class TestDefinition extends SelinuxBaseTest {
    async run() {
        // The only things we are verifying here:
        // - that we are connected
        // - that process is running in correct SELinux context

        assert(db);
        assert.eq(0, run("bash", "-c", "ps -efZ | grep -P 'system_u:system_r:mongod_t:s0[ ]+mongod'"));

        jsTest.log("success");
    }
}
