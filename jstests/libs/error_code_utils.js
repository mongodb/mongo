/**
 * Helper functions for analyzing server errors.
 */

"use strict";

function includesErrorCode(serverReply, code) {
    if (serverReply.code === code) {
        return true;
    } else if (serverReply.writeErrors) {
        for (let e of serverReply.writeErrors) {
            if (e.code === code) {
                return true;
            }
        }
    }

    return false;
}
