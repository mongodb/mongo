// Test that decimal usage is restricted when the featureCompatibilityVersion is 3.2.

(function() {
    "use strict";

    const conn = MongoRunner.runMongod({});
    assert.neq(null, conn, "mongod was unable to start up");

    const decimalDB = conn.getDB("decimal_feature_compatibility_version");
    assert.commandWorked(decimalDB.dropDatabase());
    assert.commandWorked(decimalDB.runCommand({create: "collection"}));

    const adminDB = conn.getDB("admin");

    // Ensure the featureCompatibilityVersion is 3.4.
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: "3.4"}));
    let res = adminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq("3.4", res.featureCompatibilityVersion);

    // Decimals can be inserted when the featureCompatibilityVersion is 3.4.
    assert.writeOK(decimalDB.collection.insert({a: NumberDecimal(2.0)}));

    // Ensure the featureCompatibilityVersion is 3.2.
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: "3.2"}));
    res = adminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    assert.eq("3.2", res.featureCompatibilityVersion);

    // Decimals cannot be inserted when the featureCompatibilityVersion is 3.2.
    // TODO SERVER-23972: Inserting decimals should also fail in legacy write mode when
    // featureCompatibilityVersion is 3.2.
    if (decimalDB.getMongo().writeMode() === "commands") {
        assert.writeErrorWithCode(decimalDB.collection.insert({a: NumberDecimal(2.0)}),
                                  ErrorCodes.InvalidBSON);
    }

    // Decimals cannot be used in queries when the featureCompatibilityVersion is 3.2.
    assert.throws(function() {
        decimalDB.collection.findOne({a: NumberDecimal(2.0)});
    });

    // Decimals can be read when the featureCompatibilityVersion is 3.2.
    assert.eq(decimalDB.collection.findOne().a, NumberDecimal(2.0));

    MongoRunner.stopMongod(conn);
}());
