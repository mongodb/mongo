/**
 *    Copyright (C) 2012 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/string_data.h"
#include "mongo/db/jsobj.h"

namespace mongo {

    /**
     * This class represents the layout and contents of documents contained in the
     * config.settings collection. All manipulation of documents coming from that
     * collection should be done with this class.
     *
     * Usage Example:
     *
     *     // Contact the config. 'conn' has been obtained before.
     *     DBClientBase* conn;
     *     BSONObj query = QUERY(SettingsType::exampleField("exampleFieldName"));
     *     exampleDoc = conn->findOne(SettingsType::ConfigNS, query);
     *
     *     // Process the response.
     *     SettingsType exampleType;
     *     string errMsg;
     *     if (!exampleType.parseBSON(exampleDoc, &errMsg) || !exampleType.isValid(&errMsg)) {
     *         // Can't use 'exampleType'. Take action.
     *     }
     *     // use 'exampleType'
     *
     */
    class SettingsType {
        MONGO_DISALLOW_COPYING(SettingsType);
    public:

        //
        // schema declarations
        //

        // Name of the settings collection in the config server.
        static const std::string ConfigNS;

        // Field names and types in the settings collection type.
        static const BSONField<std::string> key;
        static const BSONField<int> chunksize;
        static const BSONField<bool> balancerStopped;
        static const BSONField<BSONObj> balancerActiveWindow;
        static const BSONField<bool> shortBalancerSleep;
        static const BSONField<bool> secondaryThrottle;

        //
        // settings type methods
        //

        SettingsType();
        ~SettingsType();

        /**
         * Returns true if all the mandatory fields are present and have valid
         * representations. Otherwise returns false and fills in the optional 'errMsg' string.
         */
        bool isValid(std::string* errMsg) const;

        /**
         * Returns the BSON representation of the entry.
         */
        BSONObj toBSON() const;

        /**
         * Clears and populates the internal state using the 'source' BSON object if the
         * latter contains valid values. Otherwise sets errMsg and returns false.
         */
        bool parseBSON(const BSONObj& source, std::string* errMsg);

        /**
         * Clears the internal state.
         */
        void clear();

        /**
         * Copies all the fields present in 'this' to 'other'.
         */
        void cloneTo(SettingsType* other) const;

        /**
         * Returns a string representation of the current internal state.
         */
        std::string toString() const;

        //
        // individual field accessors
        //

        // Mandatory Fields
        void setKey(const StringData& key) {
            _key = key.toString();
            _isKeySet = true;
        }

        void unsetKey() { _isKeySet = false; }

        bool isKeySet() const { return _isKeySet; }

        // Calling get*() methods when the member is not set results in undefined behavior
        const std::string getKey() const {
            dassert(_isKeySet);
            return _key;
        }

        // Optional Fields
        void setChunksize(int chunksize) {
            _chunksize = chunksize;
            _isChunksizeSet = true;
        }

        void unsetChunksize() { _isChunksizeSet = false; }

        bool isChunksizeSet() const {
            return _isChunksizeSet || chunksize.hasDefault();
        }

        // Calling get*() methods when the member is not set and has no default results in undefined
        // behavior
        int getChunksize() const {
            if (_isChunksizeSet) {
                return _chunksize;
            } else {
                dassert(chunksize.hasDefault());
                return chunksize.getDefault();
            }
        }
        void setBalancerStopped(bool balancerStopped) {
            _balancerStopped = balancerStopped;
            _isBalancerStoppedSet = true;
        }

        void unsetBalancerStopped() { _isBalancerStoppedSet = false; }

        bool isBalancerStoppedSet() const {
            return _isBalancerStoppedSet || balancerStopped.hasDefault();
        }

        // Calling get*() methods when the member is not set and has no default results in undefined
        // behavior
        bool getBalancerStopped() const {
            if (_isBalancerStoppedSet) {
                return _balancerStopped;
            } else {
                dassert(balancerStopped.hasDefault());
                return balancerStopped.getDefault();
            }
        }
        void setBalancerActiveWindow(BSONObj& balancerActiveWindow) {
            _balancerActiveWindow = balancerActiveWindow.getOwned();
            _isBalancerActiveWindowSet = true;
        }

        void unsetBalancerActiveWindow() { _isBalancerActiveWindowSet = false; }

        bool isBalancerActiveWindowSet() const {
            return _isBalancerActiveWindowSet || balancerActiveWindow.hasDefault();
        }

        // Calling get*() methods when the member is not set and has no default results in undefined
        // behavior
        BSONObj getBalancerActiveWindow() const {
            if (_isBalancerActiveWindowSet) {
                return _balancerActiveWindow;
            } else {
                dassert(balancerActiveWindow.hasDefault());
                return balancerActiveWindow.getDefault();
            }
        }
        void setShortBalancerSleep(bool shortBalancerSleep) {
            _shortBalancerSleep = shortBalancerSleep;
            _isShortBalancerSleepSet = true;
        }

        void unsetShortBalancerSleep() { _isShortBalancerSleepSet = false; }

        bool isShortBalancerSleepSet() const {
            return _isShortBalancerSleepSet || shortBalancerSleep.hasDefault();
        }

        // Calling get*() methods when the member is not set and has no default results in undefined
        // behavior
        bool getShortBalancerSleep() const {
            if (_isShortBalancerSleepSet) {
                return _shortBalancerSleep;
            } else {
                dassert(shortBalancerSleep.hasDefault());
                return shortBalancerSleep.getDefault();
            }
        }
        void setSecondaryThrottle(bool secondaryThrottle) {
            _secondaryThrottle = secondaryThrottle;
            _isSecondaryThrottleSet = true;
        }

        void unsetSecondaryThrottle() { _isSecondaryThrottleSet = false; }

        bool isSecondaryThrottleSet() const {
            return _isSecondaryThrottleSet || secondaryThrottle.hasDefault();
        }

        // Calling get*() methods when the member is not set and has no default results in undefined
        // behavior
        bool getSecondaryThrottle() const {
            if (_isSecondaryThrottleSet) {
                return _secondaryThrottle;
            } else {
                dassert(secondaryThrottle.hasDefault());
                return secondaryThrottle.getDefault();
            }
        }

    private:
        // Convention: (M)andatory, (O)ptional, (S)pecial rule.
        std::string _key;                // (M)  key determining the type of options to use
        bool _isKeySet;
                                         // === chunksize options ===
        int _chunksize;                  // (O)  size of the chunks in our cluster
        bool _isChunksizeSet;
                                         // === balancer options ===
        bool _balancerStopped;           // (O)  balancer enabled/disabled
        bool _isBalancerStoppedSet;

        BSONObj _balancerActiveWindow;   // (O)  if present, activeWindow is an interval
        bool _isBalancerActiveWindowSet; // during the day when the balancer should
                                         // be active.
                                         // Format: { start: "08:00" , stop:
                                         // "19:30" }, strftime format is %H:%M

        bool _shortBalancerSleep;        // (O)  controls how long the balancer sleeps
        bool _isShortBalancerSleepSet;   // in some situations

        bool _secondaryThrottle;         // (O)  only migrate chunks as fast as at least
        bool _isSecondaryThrottleSet;    // one secondary can keep up with
    };

} // namespace mongo
