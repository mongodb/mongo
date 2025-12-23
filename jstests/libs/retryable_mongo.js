
/**
 * Construct a new Mongo instance, retrying multiple times in case of failure.
 * @param  {...any} args to be passed onto the Mongo constructor.
 * @returns New Mongo instance
 * @throws After maximum retries have exceeded.
 */
export default function newMongoWithRetry(...args) {
    const MAX_RETRIES = 10;
    let retryCount = 0;

    while (true) {
        try {
            return globalThis.Mongo.apply(this, args);
        } catch (error) {
            if (++retryCount >= MAX_RETRIES) {
                throw error;
            }
        }
    }
}
