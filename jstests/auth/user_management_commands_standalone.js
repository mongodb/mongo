(function() {
    'use strict';

    load('jstests/auth/user_management_commands_lib.js');

    var conn = MerizoRunner.runMerizod({auth: '', useHostname: false});
    runAllUserManagementCommandsTests(conn);
    MerizoRunner.stopMerizod(conn);
})();
