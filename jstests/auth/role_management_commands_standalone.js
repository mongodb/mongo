(function() {
    'use strict';

    load('jstests/auth/role_management_commands_lib.js');

    var conn = MerizoRunner.runMerizod({auth: '', useHostname: false});
    runAllRoleManagementCommandsTests(conn);
    MerizoRunner.stopMerizod(conn);
})();
