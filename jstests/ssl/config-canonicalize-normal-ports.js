// Make sure the psuedo-option --tlsOnNormalPorts is correctly canonicalized.

(function() {
    'use strict';

    const merizod = MerizoRunner.runMerizod({
        tlsOnNormalPorts: '',
        tlsCertificateKeyFile: 'jstests/libs/server.pem',
    });
    assert(merizod);
    assert.commandWorked(merizod.getDB('admin').runCommand({isMaster: 1}));
    MerizoRunner.stopMerizod(merizod);
})();
