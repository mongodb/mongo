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
     * config.collections collection. All manipulation of documents coming from that
     * collection should be done with this class.
     *
     * Usage Example:
     *
     *     // Contact the config. 'conn' has been obtained before.
     *     DBClientBase* conn;
     *     BSONObj query = QUERY(CollectionType::exampleField("exampleFieldName"));
     *     exampleDoc = conn->findOne(CollectionType::ConfigNS, query);
     *
     *     // Process the response.
     *     CollectionType exampleType;
     *     string errMsg;
     *     if (!exampleType.parseBSON(exampleDoc, &errMsg) || !exampleType.isValid(&errMsg)) {
     *         // Can't use 'exampleType'. Take action.
     *     }
     *     // use 'exampleType'
     *
     */
    class CollectionType {
        MONGO_DISALLOW_COPYING(CollectionType);
    public:

        //
        // schema declarations
        //

        // Name of the collections collection in the config server.
        static const std::string ConfigNS;

        // Field names and types in the collections collection type.
        static const BSONField<std::string> ns;
        static const BSONField<std::string> primary;
        static const BSONField<BSONObj> keyPattern;
        static const BSONField<bool> unique;
        static const BSONField<Date_t> updatedAt;
        static const BSONField<bool> noBalance;
        static const BSONField<OID> epoch;
        static const BSONField<bool> dropped;
        static const BSONField<OID> DEPRECATED_lastmodEpoch;
        static const BSONField<Date_t> DEPRECATED_lastmod;

        //
        // collections type methods
        //

        CollectionType();
        ~CollectionType();

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
        void cloneTo(CollectionType* other) const;

        /**
         * Returns a string representation of the current internal state.
         */
        std::string toString() const;

        //
        // individual field accessors
        //

        // Mandatory Fields
        void setNS(const StringData& ns) {
            _ns = ns.toString();
            _isNsSet = true;
        }

        void unsetNS() { _isNsSet = false; }

        bool isNSSet() const { return _isNsSet; }

        // Calling get*() methods when the member is not set results in undefined behavior
        const std::string getNS() const {
            dassert(_isNsSet);
            return _ns;
        }

        void setUpdatedAt(const Date_t updatedAt) {
            _updatedAt = updatedAt;
            _isUpdatedAtSet = true;
        }

        void unsetUpdatedAt() { _isUpdatedAtSet = false; }

        bool isUpdatedAtSet() const { return _isUpdatedAtSet; }

        // Calling get*() methods when the member is not set results in undefined behavior
        const Date_t getUpdatedAt() const {
            dassert(_isUpdatedAtSet);
            return _updatedAt;
        }

        void setEpoch(const OID epoch) {
            _epoch = epoch;
            _isEpochSet = true;
        }

        void unsetEpoch() { _isEpochSet = false; }

        bool isEpochSet() const { return _isEpochSet; }

        // Calling get*() methods when the member is not set results in undefined behavior
        const OID getEpoch() const {
            dassert(_isEpochSet);
            return _epoch;
        }

        // Optional Fields
        void setPrimary(StringData& primary) {
            _primary = primary.toString();
            _isPrimarySet = true;
        }

        void unsetPrimary() { _isPrimarySet = false; }

        bool isPrimarySet() const {
            return _isPrimarySet || primary.hasDefault();
        }

        // Calling get*() methods when the member is not set and has no default results in undefined
        // behavior
        std::string getPrimary() const {
            if (_isPrimarySet) {
                return _primary;
            } else {
                dassert(primary.hasDefault());
                return primary.getDefault();
            }
        }
        void setKeyPattern(const BSONObj& keyPattern) {
            _keyPattern = keyPattern.getOwned();
            _isKeyPatternSet = true;
        }

        void unsetKeyPattern() { _isKeyPatternSet = false; }

        bool isKeyPatternSet() const {
            return _isKeyPatternSet || keyPattern.hasDefault();
        }

        // Calling get*() methods when the member is not set and has no default results in undefined
        // behavior
        BSONObj getKeyPattern() const {
            if (_isKeyPatternSet) {
                return _keyPattern;
            } else {
                dassert(keyPattern.hasDefault());
                return keyPattern.getDefault();
            }
        }
        void setUnique(bool unique) {
            _unique = unique;
            _isUniqueSet = true;
        }

        void unsetUnique() { _isUniqueSet = false; }

        bool isUniqueSet() const {
            return _isUniqueSet || unique.hasDefault();
        }

        // Calling get*() methods when the member is not set and has no default results in undefined
        // behavior
        bool getUnique() const {
            if (_isUniqueSet) {
                return _unique;
            } else {
                dassert(unique.hasDefault());
                return unique.getDefault();
            }
        }
        void setNoBalance(bool noBalance) {
            _noBalance = noBalance;
            _isNoBalanceSet = true;
        }

        void unsetNoBalance() { _isNoBalanceSet = false; }

        bool isNoBalanceSet() const {
            return _isNoBalanceSet || noBalance.hasDefault();
        }

        // Calling get*() methods when the member is not set and has no default results in undefined
        // behavior
        bool getNoBalance() const {
            if (_isNoBalanceSet) {
                return _noBalance;
            } else {
                dassert(noBalance.hasDefault());
                return noBalance.getDefault();
            }
        }
        void setDropped(bool dropped) {
            _dropped = dropped;
            _isDroppedSet = true;
        }

        void unsetDropped() { _isDroppedSet = false; }

        bool isDroppedSet() const {
            return _isDroppedSet || dropped.hasDefault();
        }

        // Calling get*() methods when the member is not set and has no default results in undefined
        // behavior
        bool getDropped() const {
            if (_isDroppedSet) {
                return _dropped;
            } else {
                dassert(dropped.hasDefault());
                return dropped.getDefault();
            }
        }

    private:
        // Convention: (M)andatory, (O)ptional, (S)pecial rule.
        std::string _ns;     // (M)  namespace
        bool _isNsSet;
        std::string _primary;     // (O)  either/or with _keyPattern
        bool _isPrimarySet;
        BSONObj _keyPattern;     // (O)  sharding pattern if sharded
        bool _isKeyPatternSet;
        bool _unique;     // (O)  mandatory if sharded, index is unique
        bool _isUniqueSet;
        Date_t _updatedAt;     // (M)  last updated time
        bool _isUpdatedAtSet;
        bool _noBalance;     // (O)  optional if sharded, disable balancing
        bool _isNoBalanceSet;
        OID _epoch;     // (M)  disambiguates collection incarnations
        bool _isEpochSet;
        bool _dropped;     // (O)  if true, ignore this entry
        bool _isDroppedSet;
    };

} // namespace mongo
