// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <string>

namespace [[MONGO_MOD_PUBLIC]] mongo {
namespace repl {

class ReplSettings {
public:
    std::string ourSetName() const;
    bool isReplSet() const;

    /**
     * Getters
     */
    long long getOplogSizeBytes() const;
    bool isOplogSizeInitializedUsingDefault() const;
    std::string getReplSetString() const;
    bool shouldAutoInitiate() const;

    /**
     * Static getter for the 'recoverFromOplogAsStandalone' server parameter.
     */
    static bool shouldRecoverFromOplogAsStandalone();

    /**
     * Static getter for the 'skipOplogSampling' server parameter.
     */
    static bool shouldSkipOplogSampling();

    /**
     * Setters
     */
    void setOplogSizeBytes(long long oplogSizeBytes);
    void setOplogSizeInitializedUsingDefault(bool value);
    void setReplSetString(std::string replSetString);
    void setShouldAutoInitiate();

private:
    long long _oplogSizeBytes = 0;                  // --oplogSize
    bool _oplogSizeInitializedUsingDefault = true;  // true when --oplogSize was not set
    bool _shouldAutoInitiate = false;
    std::string _replSetString;  // --replSet[/<seedlist>]
};

}  // namespace repl
}  // namespace mongo
