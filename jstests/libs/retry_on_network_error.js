/**
 * Executes the specified function and if it fails due to exception, which is related to network
 * error retries the call once. If the second attempt also fails, simply throws the last
 * exception.
 *
 * Returns the return value of the input call.
 */
function retryOnNetworkError(func, numRetries = 1) {
    while (true) {
        try {
            return func();
        } catch (e) {
            if (isNetworkError(e) && numRetries > 0) {
                print("Network error occurred and the call will be retried: " +
                      tojson({error: e.toString(), stack: e.stack}));
                numRetries--;
            } else {
                throw e;
            }
        }
    }
}
