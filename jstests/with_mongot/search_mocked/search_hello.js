/**
 * Test that hello and its aliases, ismaster and isMaster, are all accepted
 * by mongotmock and that the appropriate response fields are returned.
 */
import {MongotMock} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

// Set up mongotmock.
const mongotmock = new MongotMock();
mongotmock.start();
const mongotConn = mongotmock.getConnection();

function checkResponseFields(commandString) {
    jsTestLog("Testing " + commandString + " for MongotMock");
    const commandResponse = assert.commandWorked(mongotConn.adminCommand(commandString));
    assert.eq(commandResponse.ismongot,
              true,
              "ismongot is not true, command response: " + tojson(commandResponse));

    if (commandString === "hello") {
        assert.eq(commandResponse.ismaster,
                  undefined,
                  "ismaster is not undefined, command response: " + tojson(commandResponse));
        assert.eq(commandResponse.isWritablePrimary,
                  true,
                  "isWritablePrimary is not true, command response: " + tojson(commandResponse));
    } else {
        assert.eq(commandResponse.ismaster,
                  true,
                  "ismaster is not true, command response: " + tojson(commandResponse));
        assert.eq(
            commandResponse.isWritablePrimary,
            undefined,
            "isWritablePrimary is not undefined, command response: " + tojson(commandResponse));
    }
}

checkResponseFields("ismaster");
checkResponseFields("isMaster");
checkResponseFields("hello");

mongotmock.stop();
