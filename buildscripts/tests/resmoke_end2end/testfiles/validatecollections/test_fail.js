const t = db.test_validate_failure;
t.drop();
const adminDB = db.getSiblingDB("admin");

assert.commandWorked(t.insert({_id: 1, x: 1}));

assert.commandWorked(
    adminDB.runCommand({
        configureFailPoint: "failCommand",
        mode: "alwaysOn",
        data: {
            errorCode: ErrorCodes.InternalError,
            failCommands: ["validate"],
        },
    }),
);
