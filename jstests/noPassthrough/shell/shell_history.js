// Test that when running the shell for the first time creates the ~/.dbshell file, and it has
// appropriate permissions (where relevant).

// Use dataPath because it includes the trailing "/" or "\".
let tmpHome = MongoRunner.dataPath;
// Ensure it exists and is a dir (eg. if running without resmoke.py and /data/db doesn't exist).
mkdir(tmpHome);
removeFile(tmpHome + ".dbshell");

let args = [];
let cmdline = "mongo --nodb";
let redirection = "";
let env = {};
if (_isWindows()) {
    args.push("cmd.exe");
    args.push("/c");
    cmdline = cmdline.replace("mongo", "mongo.exe");

    // Input is set to NUL.  The output must also be redirected to NUL, otherwise running the
    // jstest manually has strange terminal IO behaviour.
    redirection = "< NUL > NUL";

    // USERPROFILE set to the tmp homedir.
    // Since NUL is a character device, isatty() will return true, which means that .mongorc.js
    // will be created in the HOMEDRIVE + HOMEPATH location, so we must set them also.
    let tmpHomeDrive, tmpHomePath;
    if (tmpHome.match("^[a-zA-Z]:")) {
        tmpHomeDrive = tmpHome.substr(0, 2);
        tmpHomePath = tmpHome.substr(2);
    } else {
        let _pwd = pwd();
        assert(_pwd.match("^[a-zA-Z]:"), "pwd must include drive");
        tmpHomeDrive = _pwd.substr(0, 2);
        tmpHomePath = tmpHome;
    }
    env = {USERPROFILE: tmpHome, HOMEDRIVE: tmpHomeDrive, HOMEPATH: tmpHomePath};
} else {
    args.push("sh");
    args.push("-c");

    // Use the mongo shell from the $PATH, Resmoke sets $PATH to
    // include all the mongo binaries first.
    /* eslint-disable-next-line */
    cmdline = cmdline;

    // Set umask to 0 prior to running the shell.
    cmdline = "umask 0 ; " + cmdline;

    // stdin is /dev/null.
    redirection = "< /dev/null";

    // HOME set to the tmp homedir.
    if (!tmpHome.startsWith("/")) {
        tmpHome = pwd() + "/" + tmpHome;
    }
    env = {HOME: tmpHome};
}

// Add redirection to cmdline, and add cmdline to args.
cmdline += " " + redirection;
args.push(cmdline);
jsTestLog("Running args:\n    " + tojson(args) + "\nwith env:\n    " + tojson(env));
let pid = _startMongoProgram({args, env});
let rc = waitProgram(pid);

assert.eq(rc, 0);

let files = listFiles(tmpHome);
jsTestLog(tojson(files));

let findFile = function (baseName) {
    for (let i = 0; i < files.length; i++) {
        if (files[i].baseName === baseName) {
            return files[i];
        }
    }
    return undefined;
};

let targetFile = ".dbshell";
let file = findFile(targetFile);

assert.neq(typeof file, "undefined", targetFile + " should exist, but it doesn't");
assert.eq(file.isDirectory, false, targetFile + " should not be a directory, but it is");
assert.eq(file.size, 0, targetFile + " should be empty, but it isn't");

if (!_isWindows()) {
    // On Unix, check that the file has the correct mode (permissions).
    // The shell has no way to stat a file.
    // There is no stat utility in POSIX.
    // `ls -l` is POSIX, so this is the best that we have.
    // Check for exactly "-rw-------".
    clearRawMongoProgramOutput();
    let rc = runProgram("ls", "-l", file.name);
    assert.eq(rc, 0);

    let output = rawMongoProgramOutput(".*");
    let fields = output.split(" ");
    // First field is the prefix, second field is the `ls -l` permissions.
    assert.eq(fields[1].substr(0, 10), "-rw-------", targetFile + " has bad permissions");
}
