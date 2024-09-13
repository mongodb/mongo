/**
 * Miscellaneous utilities which are commonly useful when dealing
 * with success/failure, and retries.
 */

export function hasError(res) {
    return res.ok !== 1 || res.writeErrors || (res.hasOwnProperty("nErrors") && res.nErrors != 0);
}

export function hasWriteConcernError(res) {
    return res.hasOwnProperty("writeConcernError");
}

export function isSuccess(res) {
    return !(hasError(res) || hasWriteConcernError(res));
}

/**
 * Wrapper for a status or an exception.
 *
 * Typical usage:
 *
 *   let res = Result.wrap(() => mightReturnOrThrow());
 *   if (res.ok()) {...}
 *   return res.unwrap(); // Returns or throws.
 */
export class Result {
    constructor(status, exception) {
        // Status or exception must be present.
        assert((status != null) != (exception != null));
        this.status = status;
        this.exception = exception;
    }
    static status(status) {
        let res = new Result(status, null);
        return res;
    }

    static exception(exception) {
        let res = new Result(null, exception);
        return res;
    }

    static wrap(callable) {
        try {
            return Result.status(callable());
        } catch (e) {
            return Result.exception(e);
        }
    }

    ok() {
        return this.status != null && isSuccess(this.status);
    }

    unwrap() {
        if (this.exception != null) {
            throw this.exception;
        }
        return this.status;
    }
    apply(callback) {
        if (this.exception != null) {
            return callback(this.exception);
        }
        return callback(this.status);
    }
    dispatch(onStatus, onException) {
        if (this.exception != null) {
            return onException(this.exception);
        }
        return onStatus(this.status);
    }
}

/**
 * Utility to simplify looping until X milliseconds elapse;
 * a common pattern when retrying operations.
 *
 * let helper = new RetryTracker(3000);
 * for (let _ of helper) {
 *     ... // E.g., try a command. Will loop until break or exceeding the time limit.
 * }
 * The timer starts from creation of the helper.
 */
export class RetryTracker {
    constructor(timeout) {
        this.retries = 0;
        this.timeout = timeout;
        this.startTime = Date.now();
        this.timeoutExceeded = false;
    }

    * [Symbol.iterator]() {
        while (true) {
            let elapsed = Date.now() - this.startTime;
            if (elapsed > this.timeout) {
                this.timeoutExceeded = true;
                break;
            }
            yield {retries: this.retries++, remainingTime: this.timeout - elapsed};
        }
    }
}
