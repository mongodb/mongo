/**
 * Helper functions for document validation.
 */

/**
 * Assert that a command fails with a DocumentValidationFailure, and verify that the
 * 'errInfo' field is propogated as a part of the doc validation failure.
 */
function assertDocumentValidationFailure(res, coll) {
    assert.commandFailedWithCode(res, ErrorCodes.DocumentValidationFailure, tojson(res));
    if (coll.getMongo().writeMode() === "commands") {
        if (res instanceof BulkWriteResult) {
            const errors = res.getWriteErrors();
            for (const error of errors) {
                assert(error.hasOwnProperty("errInfo"), tojson(error));
                assert.eq(typeof error["errInfo"], "object", tojson(error));
            }
        } else {
            const error = res instanceof WriteResult ? res.getWriteError() : res;
            assert(error.hasOwnProperty("errInfo"), tojson(error));
            assert.eq(typeof error["errInfo"], "object", tojson(error));
        }
    }
}
