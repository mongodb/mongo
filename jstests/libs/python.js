// Helper for finding the local python binary.

function getPython3Binary() {
    'use strict';

    let cmd = '/opt/mongodbtoolchain/v3/bin/python3';
    if (_isWindows()) {
        const paths = ["c:/python36/python.exe", "c:/python/python36/python.exe"];
        for (let p of paths) {
            if (fileExists(p)) {
                cmd = p;
                break;
            }
        }
    }

    assert(fileExists(cmd), "Python3 interpreter not found");
    return cmd;
}
