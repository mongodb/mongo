// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <string>
#include <string_view>

/**
 * The 'redact' methods defined below should be used to redact possibly sensitive
 * information when operating the server in 'redact' mode.
 *
 * The performance impact of calling redact when not in 'redact' mode should be neglectible.
 *
 * The 'redact' methods are designed to be used as part of our log streams
 * log(), LOG(), warning(), error(), severe() similar to the example below.
 *
 * log() << "My sensitive query is: " << query;
 * log() << "My sensitive query is: " << redact(query);
 */

namespace mongo {

class BSONObj;
class Status;
class DBException;

/**
 *  In 'redact' mode (or if forceRedaction is true) replace all values with '###' and keep keys
 *  intact. In normal mode return objectToRedact.toString().
 */
[[MONGO_MOD_PUBLIC]] BSONObj redact(const BSONObj& objectToRedact, bool forceRedaction = false);

/**
 *  In 'redact' mode (or if forceRedaction is true) return '###'.
 *  In normal mode return stringToRedact.
 */
[[MONGO_MOD_PUBLIC]] std::string_view redact(std::string_view stringToRedact,
                                             bool forceRedaction = false);
[[MONGO_MOD_PUBLIC]] inline std::string_view redact(const char* stringToRedact,
                                                    bool forceRedaction = false) {
    return redact(std::string_view(stringToRedact), forceRedaction);
}
[[MONGO_MOD_PUBLIC]] inline std::string_view redact(const std::string& stringToRedact,
                                                    bool forceRedaction = false) {
    return redact(std::string_view(stringToRedact), forceRedaction);
}

/**
 *  In 'redact' mode (or if forceRedaction is true) keep status code and replace reason with '###'.
 *  In normal mode return statusToRedact.toString().
 */
[[MONGO_MOD_PUBLIC]] std::string redact(const Status& statusToRedact, bool forceRedaction = false);

/**
 * In 'redact' mode (or if forceRedaction is true) keep exception type and replace causedBy
 * with '###'. In normal mode return exceptionToRedact.toString().
 */
[[MONGO_MOD_PUBLIC]] std::string redact(const DBException& exceptionToRedact,
                                        bool forceRedaction = false);

}  // namespace mongo
