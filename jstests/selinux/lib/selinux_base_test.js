'use strict';

class SelinuxBaseTest {
    get config() {
        return {};
    }

    // Notice: private definitions, e.g.: #sudo() are not
    // recognized by js linter, so leaving this declaration public
    sudo(script) {
        return run("sudo", "--non-interactive", "bash", "-c", script);
    }

    setup() {
    }

    teardown() {
    }

    run() {
        assert("override this function");
    }
}
