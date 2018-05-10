/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <string>

#include "mongo/db/jsobj.h"

namespace mongo {
namespace repl {

extern int maxSyncSourceLagSecs;
extern double replElectionTimeoutOffsetLimitFraction;

class ReplSettings {
public:
    // Allow index prefetching to be turned on/off
    enum class IndexPrefetchConfig {
        UNINITIALIZED = 0,
        PREFETCH_NONE = 1,
        PREFETCH_ID_ONLY = 2,
        PREFETCH_ALL = 3
    };

    std::string ourSetName() const;
    bool usingReplSets() const;

    /**
     * Getters
     */
    long long getOplogSizeBytes() const;
    std::string getReplSetString() const;

    /**
     * Static getter for the 'recoverFromOplogAsStandalone' server parameter.
     */
    static bool shouldRecoverFromOplogAsStandalone();

    /**
     * Note: _prefetchIndexMode is initialized to UNINITIALIZED by default.
     * To check whether _prefetchIndexMode has been set to a valid value, call
     * isPrefetchIndexModeSet().
     */
    IndexPrefetchConfig getPrefetchIndexMode() const;

    /**
      * Checks that _prefetchIndexMode has been set.
      */
    bool isPrefetchIndexModeSet() const;

    /**
     * Setters
     */
    void setOplogSizeBytes(long long oplogSizeBytes);
    void setReplSetString(std::string replSetString);
    void setPrefetchIndexMode(std::string prefetchIndexModeString);

private:
    long long _oplogSizeBytes = 0;  // --oplogSize

    std::string _replSetString;  // --replSet[/<seedlist>]

    // --indexPrefetch
    IndexPrefetchConfig _prefetchIndexMode = IndexPrefetchConfig::UNINITIALIZED;
};

}  // namespace repl
}  // namespace mongo
