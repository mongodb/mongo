/**
 * Test that the --version command always outputs the following format to avoid downstream
 * breakages:
 *
 * sh*| ... version ...
 * sh*| Build Info: {
 *          ...
 * sh*| }
 */

const dbVersionMatch = /sh([0-9]{1,10})\| .+ version .+/;
const buildInfoMatch = /sh([0-9]{1,10})\| Build Info: {/;

const testVersionOutput = (exePath) => {
    clearRawMongoProgramOutput();

    runNonMongoProgram(exePath, "--version");

    const out = rawMongoProgramOutput().split('\n');

    assert.neq(out[0].match(dbVersionMatch), null);
    assert.neq(out[1].match(buildInfoMatch), null);
};

testVersionOutput(MongoRunner.getMongodPath());
testVersionOutput(MongoRunner.getMongosPath());
testVersionOutput(MongoRunner.getMongoShellPath());
