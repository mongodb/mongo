/**
 * kill_session.js
 *
 * State function that kills a random session from config.system.sessions.
 */
import {KilledSessionUtil} from "jstests/libs/killed_session_util.js";

export function killSession(db, collName) {
    print("Starting killSession");
    let ourSessionWasKilled;
    do {
        ourSessionWasKilled = false;

        try {
            let res = db.adminCommand({refreshLogicalSessionCacheNow: 1});
            if (res.ok === 1) {
                assert.commandWorked(res);
            } else if (res.code === 18630 || res.code === 18631) {
                // Refreshing the logical session cache may trigger sharding the sessions
                // collection, which can fail with 18630 or 18631 if its session is killed while
                // running DBClientBase::getCollectionInfos() or DBClientBase::getIndexSpecs(),
                // respectively. This means the collection is not set up, so retry.
                ourSessionWasKilled = true;
                continue;
            } else {
                assert.commandFailedWithCode(
                    res,
                    [ErrorCodes.DuplicateKey, ErrorCodes.WriteConcernTimeout],
                    'unexpected error code: ' + res.code + ': ' + res.message);
            }

            const sessionToKill = db.getSiblingDB("config").system.sessions.aggregate([
                {$listSessions: {}},
                {$match: {"_id.id": {$ne: db.getSession().getSessionId().id}}},
                {$sample: {size: 1}},
            ]);

            if (sessionToKill.toArray().length === 0) {
                break;
            }

            const sessionUUID = sessionToKill.toArray()[0]._id.id;
            res = db.runCommand({killSessions: [{id: sessionUUID}]});
            assert.commandWorked(res);
        } catch (e) {
            if (KilledSessionUtil.isKilledSessionCode(e.code)) {
                // This session was killed when running either listSessions or killSesssions.
                // We should retry.
                ourSessionWasKilled = true;
                continue;
            }

            throw e;
        }
    } while (ourSessionWasKilled);
    print("Finished killSession");
}
