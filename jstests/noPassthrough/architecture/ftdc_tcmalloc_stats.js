// Validate FTDC correctly gathers tcmalloc stats

import {setParameter, verifyGetDiagnosticData} from "jstests/libs/ftdc.js";

let m = MongoRunner.runMongod();
const adminDb = m.getDB('admin');

// Ensure that on a low verbosity, the formatted stats string is not included and the other expected
// fields are present.
let validateTcmallocStats = (stats) => {
    return stats.hasOwnProperty("tcmalloc") && !stats.tcmalloc.hasOwnProperty("formattedString");
};

let doc;
assert.soon(() => {
    doc = verifyGetDiagnosticData(adminDb);
    return doc.hasOwnProperty("serverStatus");
});

// If we have the tcmalloc section, then the server was compiled with either the gperf or google
// tcmalloc, and so we can continue testing those properties.
if (doc.serverStatus.hasOwnProperty("tcmalloc")) {
    // On regular FTDC startup, we do not include the tcmalloc stats string.
    validateTcmallocStats(doc.serverStatus.tcmalloc);

    assert.commandWorked(setParameter(adminDb, {"diagnosticDataCollectionVerboseTCMalloc": true}));

    // Even when the verbose FTDC setting is on, we still do not include the stats string.
    assert.soon(() => {
        let doc = verifyGetDiagnosticData(adminDb);
        if (validateTcmallocStats(doc.serverStatus.tcmalloc)) {
            if (doc.serverStatus.tcmalloc.hasOwnProperty("usingPerCPUCaches")) {
                // Running New TCMalloc-- if cpu caches are on, we include the cpuCache verbose
                // info.
                return !doc.serverStatus.tcmalloc.usingPerCPUCaches ||
                    doc.serverStatus.tcmalloc.tcmalloc.hasOwnProperty("cpuCache");
            } else {
                // Running Old TCMalloc-- we include the size_classes verbose info.
                return doc.serverStatus.tcmalloc.tcmalloc.hasOwnProperty("size_classes");
            }
        }

        return false;
    });

    // We can access the stats string through serverStatus with verbosity 3, regardless of the FTDC
    // settings.
    const ss = adminDb.serverStatus({tcmalloc: 3});
    assert.neq(ss.tcmalloc.tcmalloc.formattedString, null, tojson(ss.tcmalloc));
}

MongoRunner.stopMongod(m);
