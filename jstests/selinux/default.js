// This test does not run any code. As long as mongod is
// up and running, it is successful

'use strict';

load('jstests/selinux/lib/selinux_base_test.js');

class TestDefinition extends SelinuxBaseTest {
    run() {
        // The only things we are verifying here:
        // - that we are connected
        // - that process is running in correct SELinux context

        assert(db);
        assert.eq(0,
                  run("bash", "-c", "ps -efZ | grep -P 'system_u:system_r:mongod_t:s0[ ]+mongod'"));

        jsTest.log("success");
    }
}
