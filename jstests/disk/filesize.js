// Test for SERVER-7430: Warning about smallfiles should include filename

// `--smallfiles` is mmap only.
// @tags: [requires_mmapv1]
var baseName = "filesize";

// Start bongod with --smallfiles
var m = BongoRunner.runBongod({nojournal: "", smallfiles: ""});

var db = m.getDB(baseName);

// Skip on 32 bits, since 32-bit servers don't warn about small files
if (db.serverBuildInfo().bits == 32) {
    print("Skip on 32-bit");
} else {
    // Restart bongod without --smallFiles
    BongoRunner.stopBongod(m);
    m = BongoRunner.runBongod({
        restart: true,
        cleanData: false,
        dbpath: m.dbpath,
        port: m.port,
        nojournal: "",
    });

    db = m.getDB(baseName);
    var log = db.adminCommand({getLog: "global"}).log;

    // Find log message like:
    // "openExisting file size 16777216 but
    // mmapv1GlobalOptions.smallfiles=false: /data/db/filesize/local.0"
    var found = false, logline = '';
    for (i = log.length - 1; i >= 0; i--) {
        logline = log[i];
        if (logline.indexOf("openExisting file") >= 0 && logline.indexOf("local.0") >= 0) {
            found = true;
            break;
        }
    }

    assert(found);
}
