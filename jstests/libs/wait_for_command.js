const waitForCommand = function(waitingFor, opFilter, myDB) {
    let opId = -1;
    assert.soon(function() {
        print(`Checking for ${waitingFor}`);
        const curopRes = myDB.currentOp();
        assert.commandWorked(curopRes);
        const foundOp = curopRes["inprog"].filter(opFilter);

        if (foundOp.length == 1) {
            opId = foundOp[0]["opid"];
        }
        return (foundOp.length == 1);
    });
    return opId;
};
