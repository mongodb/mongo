#ifndef API_CONST_H
#define API_CONST_H

/* Define all constants related to WiredTiger APIs and testing. */
namespace test_harness {

static const char *CONNECTION_CREATE = "create";
static const char *COLLECTION_COUNT = "collection_count";
static const char *DURATION_SECONDS = "duration_seconds";
static const char *KEY_COUNT = "key_count";
static const char *READ_THREADS = "read_threads";
static const char *VALUE_SIZE = "value_size";
static const char *TRACKING_COLLECTION = "table:tracking";

} // namespace test_harness

#endif
