(function() {
    "use strict";
    const md = MerizoRunner.runMerizod({port: "27017", dbpath: MerizoRunner.dataPath});
    assert.neq(null, md, "unable to start merizod");
    const targetURI = 'merizodb+srv://test1.test.build.10gen.cc./?ssl=false';
    const exitCode = runMerizoProgram('merizo', targetURI, '--eval', ';');
    assert.eq(exitCode, 0, "Failed to connect with a `merizodb+srv://` style URI.");
    MerizoRunner.stopMerizod(md);
})();
