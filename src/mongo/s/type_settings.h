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
     *     unique_ptr<DbClientCursor> cursor;
     *     BSONObj query = QUERY(SettingsType::key("balancer"));
     *     cursor.reset(conn->query(SettingsType::ConfigNS, query, ...));
     *
     *     // Process the response.
     *     while (cursor->more()) {
     *         settingsDoc = cursor->next();
     *         SettingsType settings;
     *         settings.fromBSON(settingsDoc);
     *         if (! settings.isValid()) {
     *             // Can't use 'settings'. Take action.
     *         }
     *         // use 'settings'
     *     }
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
        static BSONField<string> key;                     // key determining the type of setting
                                                          // document this is
        // === chunksize options ===
        static BSONField<int> chunksize;                  // size of the chunks in our cluster
        // === balancer options ===
        static BSONField<bool> balancerStopped;           // balancer enabled/disabled
        static BSONField<BSONObj> balancerActiveWindow;   // if present, activeWindow is an interval
                                                          // during the day when the balancer should
                                                          // be active.
                                                          // Format: { start: "08:00" , stop:
                                                          // "19:30" }, strftime format is %H:%M
        static BSONField<bool> shortBalancerSleep;        // controls how long the balancer sleeps
                                                          // in some situations
        static BSONField<bool> secondaryThrottle;         // only migrate chunks as fast as at least
                                                          // one secondary can keep up with

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
        bool parseBSON(BSONObj source, std::string* errMsg);

        /**
         * Clears the internal state.
         */
        void clear();

        /**
         * Copies all the fields present in 'this' to 'other'.
         */
        void cloneTo(SettingsType* other);

        /**
         * Returns a string representation of the current internal state.
         */
        std::string toString() const;

        //
        // individual field accessors
        //

        void setKey(const StringData& key) { _key = key.toString(); }
        const std::string& getKey() const { return _key; }

        void setChunksize(int chunksize) { _chunksize = chunksize; }
        int getChunksize() const { return _chunksize; }

        void setBalancerStopped(bool balancerStopped) { _balancerStopped = balancerStopped; }
        bool getBalancerStopped() const { return _balancerStopped; }

        void setBalancerActiveWindow(const BSONObj balancerActiveWindow) {
            _balancerActiveWindow = balancerActiveWindow.getOwned();
        }
        BSONObj getBalancerActiveWindow() const { return _balancerActiveWindow; }

        void setShortBalancerSleep(bool shortBalancerSleep) {
            _shortBalancerSleep = shortBalancerSleep;
        }
        bool getShortBalancerSleep() const { return _shortBalancerSleep; }

        void setSecondaryThrottle(bool secondaryThrottle) {
            _secondaryThrottle = secondaryThrottle;
        }
        bool getSecondaryThrottle() const { return _secondaryThrottle; }

    private:
        // Convention: (M)andatory, (O)ptional, (S)pecial rule.
        std::string _key;              // (M) key determining the type of options to use
                                       // === chunksize options ===
        int _chunksize;                // (S) size of the chunks in our cluster
                                       // === balancer options ===
        bool _balancerStopped;         // (O) balancer enabled/disabled
        BSONObj _balancerActiveWindow; // (O) if present, activeWindow is an interval
                                       // during the day when the balancer should
                                       // be active.
                                       // Format: { start: "08:00" , stop:
                                       // "19:30" }, strftime format is %H:%M
        bool _shortBalancerSleep;      // (O) controls how long the balancer sleeps
                                       // in some situations
        bool _secondaryThrottle;       // (O) only migrate chunks as fast as at least
                                       // one secondary can keep up with
    };

}  // namespace mongo
