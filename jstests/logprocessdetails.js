/**
 * SERVER-7140 test. Checks that process info is re-logged on log rotation
 */

doTest = function(){
    var log = db.adminCommand({ getLog: 'global'});
    //this regex will need to change if output changes
    var re = new RegExp(".*conn.*options.*");

    assert.neq(null, log);
    var lineCount = log.totalLinesWritten;
    assert.neq(0, lineCount);

    var result = db.adminCommand({ logRotate: 1});
    assert.eq(1, result.ok);

    var log2 = db.adminCommand({ getLog: 'global'});
    assert.neq(null, log2);
    assert.gte(log2.totalLinesWritten, lineCount);

    var lastLineFound = re.exec(log2.log.pop());
    assert.neq(null, lastLineFound);
}

doTest();
