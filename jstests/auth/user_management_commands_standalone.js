(function() {
    'use strict';

    load('jstests/auth/user_management_commands_lib.js');

    var conn = BongoRunner.runBongod({auth: '', useHostname: false});
    runAllUserManagementCommandsTests(conn);
    BongoRunner.stopBongod(conn.port);
})();
