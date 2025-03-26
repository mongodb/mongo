// Test that mkcert.py generates certificates deterministically, and that pkcs12 certificates, while
// not deterministic, can be generated.
import {getPython3Binary} from "jstests/libs/python.js";

const now = new Date();
// Note - month is 0-indexed, so 11 is December.
if (now.getMonth() == 11 && now.getDate() == 31 && now.getHours() == 23) {
    jsTestLog(
        "Deterministic certificate generation relies on the current year being constant; skipping test as there is less than an hour until the year changes.");
    quit();
}

// Print the openssl version to help with debugging.
clearRawMongoProgramOutput();
assert.eq(runNonMongoProgram("openssl", "version"), 0);
jsTest.log(rawMongoProgramOutput(".*"));

const basedir = MongoRunner.dataPath + "certs/";
const genpath = basedir + "generated/";
mkdir(genpath);

// Run mkcert, and ensure it succeeds and that expected results are generated successfully.
jsTest.log("Running mkcert");
clearRawMongoProgramOutput();
let res = runNonMongoProgram(getPython3Binary(), "-m", "x509.mkcert", "--mkcrl", "-o", genpath);
jsTest.log(rawMongoProgramOutput(".*"));
assert.eq(res, 0);
assert(fileExists(genpath + "ca.pem"));
assert(fileExists(genpath + "crl.pem"));

// Run mkcert again, to a different path.
const genpath2 = basedir + "generated2/";
mkdir(genpath2);
jsTest.log("Running mkcert again");
res = runNonMongoProgram(getPython3Binary(), "-m", "x509.mkcert", "--mkcrl", "-o", genpath2);
assert.eq(res, 0);

// Diff the two generation paths to make sure the contents of the paths are identical.
jsTest.log("Running diff");
clearRawMongoProgramOutput();
res = runNonMongoProgram("diff", "-r", genpath, genpath2);
assert.eq(res, 0);
const diffout = rawMongoProgramOutput(".*").trim();
assert.eq("", diffout, diffout);

// Run mkcert on the apple-certs.yml definitions file, which contains pkcs12 certificates, and
// ensure a .pfx file was generated.
jsTest.log("Running apple certs");
res = runNonMongoProgram(
    getPython3Binary(), "-m", "x509.mkcert", "-o", genpath, "--config", "x509/apple-certs.yml");
assert.eq(res, 0);
assert(fileExists(genpath + "macos-trusted-server.pfx"));