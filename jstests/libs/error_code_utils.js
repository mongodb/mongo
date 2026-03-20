/**
 * Helper functions for analyzing server errors.
 */

export function includesErrorCode(serverReply, code) {
    if (serverReply.code === code) {
        return true;
    } else if (serverReply.writeErrors) {
        for (let e of serverReply.writeErrors) {
            if (e.code === code) {
                return true;
            }
        }
    } else if (serverReply instanceof BulkWriteError && serverReply.hasWriteErrors()) {
        for (let e of serverReply.getWriteErrors()) {
            if (e.code === code) {
                return true;
            }
        }
    }

    return false;
}
