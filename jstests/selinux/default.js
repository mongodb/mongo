// This test does not run any code. As long as mongod is
// up and running, it is successful

import {SelinuxBaseTest} from "jstests/selinux/lib/selinux_base_test.js";

export class TestDefinition extends SelinuxBaseTest {
    async run() {
        // The only things we are verifying here:
        // - that we are connected
        // - that process is running in correct SELinux context

        assert(db);
        jsTest.log("checking mongod service is still running..");
        assert.eq(0, run("bash", "-c", "ps -efZ | grep -P 'mongod[ ]+[0-9]+'"));

        jsTest.log("checking mongod service has the correct security label..");
        assert.eq(0, run("bash", "-c", "ps -efZ | grep -P 'system_u:system_r:mongod_t:s0[ ]+mongod'"));

        jsTest.log("success");
    }
}
