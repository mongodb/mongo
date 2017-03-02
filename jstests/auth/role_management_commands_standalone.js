(function() {
    'use strict';

    load('jstests/auth/role_management_commands_lib.js');

    var conn = BongoRunner.runBongod({auth: '', useHostname: false});
    runAllRoleManagementCommandsTests(conn);
    BongoRunner.stopBongod(conn.port);
})();
