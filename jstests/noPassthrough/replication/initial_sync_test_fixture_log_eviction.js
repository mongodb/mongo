/**
 * Tests that InitialSyncTest detects that initial sync has paused at its synchronization
 * failpoints even when the paused node's RamLog no longer contains the "fail point enabled"
 * message.
 *
 * @tags: [requires_replication]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {InitialSyncTest} from "jstests/replsets/libs/initial_sync_test.js";

describe("InitialSyncTest step()", function () {
    before(function () {
        this.initialSyncTest = new InitialSyncTest(jsTestName());
        this.primary = this.initialSyncTest.getPrimary();
        assert.commandWorked(this.primary.getDB("test").coll.insert({_id: 0}));

        // Restarts the secondary into initial sync, paused at the first synchronization point.
        assert(!this.initialSyncTest.step());
        this.secondary = this.initialSyncTest.getSecondary();

        // Make the paused secondary's RamLog useless for detecting the pause: keep a single line
        // and log every incoming command at COMMAND verbosity 2, so the "fail point enabled"
        // message is always evicted by the time getLog runs.
        this.originalRamLogMaxLines = assert.commandWorked(
            this.secondary.adminCommand({getParameter: 1, ramLogMaxLines: 1}),
        ).ramLogMaxLines;
        assert.commandWorked(this.secondary.adminCommand({setParameter: 1, ramLogMaxLines: 1}));
        assert.commandWorked(this.secondary.setLogLevel(2, "command"));
    });

    after(function () {
        assert.commandWorked(this.secondary.setLogLevel(0, "command"));
        assert.commandWorked(
            this.secondary.adminCommand({
                setParameter: 1,
                ramLogMaxLines: this.originalRamLogMaxLines,
            }),
        );
        this.initialSyncTest.stop();
    });

    it("detects pauses without relying on the RamLog", function () {
        // Each step() resumes initial sync and must notice that it paused again.
        let completed = false;
        try {
            for (let i = 0; i < 1000 && !completed; i++) {
                completed = this.initialSyncTest.step();
            }
        } catch (e) {
            // Release the paused secondary so that the replica set can shut down.
            this.initialSyncTest.fail();
            throw e;
        }
        assert(completed, "initial sync did not complete");

        this.secondary.setSecondaryOk();
        assert.eq(
            [{_id: 0}],
            this.secondary.getDB("test").coll.find().toArray(),
            "secondary did not sync the primary's data",
        );
    });
});
