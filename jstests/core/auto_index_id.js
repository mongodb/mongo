assert.commandFailed(
        db.getSiblingDB("auto_index_id_test_db").runCommand({create: "test01", autoIndexId: false})
        );

assert.commandWorked(
        db.getSiblingDB("auto_index_id_test_db").runCommand({create: "test02", autoIndexId: true})
        );

assert.commandWorked(
        db.getSiblingDB("auto_index_id_test_db").runCommand({create: "test03"})
        );



