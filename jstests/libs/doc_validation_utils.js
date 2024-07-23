/**
 * Helper functions for document validation.
 */

/**
 * Assert that a command fails with a DocumentValidationFailure, and verify that the
 * 'errInfo' field is propogated as a part of the doc validation failure.
 */
export function assertDocumentValidationFailure(res, coll) {
    assert.commandFailedWithCode(res, ErrorCodes.DocumentValidationFailure, tojson(res));
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

/**
 * Verifies that the logs contain DocumentValidationFailure.
 */
export function assertDocumentValidationFailureCheckLogs(db) {
    checkLog.contains(db, '"codeName":"DocumentValidationFailure"');
}

/**
 * Verifies that validation failed.
 */
export function assertFailsValidation(res) {
    assert.writeError(res);
    assert.eq(res.getWriteError().code, ErrorCodes.DocumentValidationFailure);
}
