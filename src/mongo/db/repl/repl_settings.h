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

#include <set>
#include <string>

#include "mongo/db/jsobj.h"
#include "mongo/util/concurrency/mutex.h"

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
    bool isSlave() const;
    bool isMaster() const;
    bool isFastSyncEnabled() const;
    bool isAutoResyncEnabled() const;
    bool isMajorityReadConcernEnabled() const;
    Seconds getSlaveDelaySecs() const;
    int getPretouch() const;
    long long getOplogSizeBytes() const;
    std::string getSource() const;
    std::string getOnly() const;
    std::string getReplSetString() const;

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
    void setSlave(bool slave);
    void setMaster(bool master);
    void setFastSyncEnabled(bool fastSyncEnabled);
    void setAutoResyncEnabled(bool autoResyncEnabled);
    void setMajorityReadConcernEnabled(bool majorityReadConcernEnabled);
    void setSlaveDelaySecs(int slaveDelay);
    void setPretouch(int pretouch);
    void setOplogSizeBytes(long long oplogSizeBytes);
    void setSource(std::string source);
    void setOnly(std::string only);
    void setReplSetString(std::string replSetString);
    void setPrefetchIndexMode(std::string prefetchIndexModeString);

private:
    /**
     * true means we are master and doing replication.  If we are not writing to oplog, this won't
     * be true.
     */
    bool _master = false;

    // replication slave? (possibly with slave)
    bool _slave = false;

    bool _fastSyncEnabled = false;
    bool _autoResyncEnabled = false;
    Seconds _slaveDelaySecs = Seconds(0);
    long long _oplogSizeBytes = 0;  // --oplogSize

    /**
     * True means that the majorityReadConcern feature is enabled, either explicitly by the user or
     * implicitly by a requiring feature such as CSRS. It does not mean that the storage engine
     * supports snapshots or that the snapshot thread is running. Those are tracked separately.
     */
    bool _majorityReadConcernEnabled = false;

    // for master/slave replication
    std::string _source;  // --source
    std::string _only;    // --only
    int _pretouch = 0;    // --pretouch for replication application (experimental)

    std::string _replSetString;  // --replSet[/<seedlist>]

    // --indexPrefetch
    IndexPrefetchConfig _prefetchIndexMode = IndexPrefetchConfig::UNINITIALIZED;
};

}  // namespace repl
}  // namespace mongo
