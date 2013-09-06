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
     * config.shards collection. All manipulation of documents coming from that
     * collection should be done with this class.
     *
     * Usage Example:
     *
     *     // Contact the config. 'conn' has been obtained before.
     *     DBClientBase* conn;
     *     BSONObj query = QUERY(ShardType::exampleField("exampleFieldName"));
     *     exampleDoc = conn->findOne(ShardType::ConfigNS, query);
     *
     *     // Process the response.
     *     ShardType exampleType;
     *     string errMsg;
     *     if (!exampleType.parseBSON(exampleDoc, &errMsg) || !exampleType.isValid(&errMsg)) {
     *         // Can't use 'exampleType'. Take action.
     *     }
     *     // use 'exampleType'
     *
     */
    class ShardType {
        MONGO_DISALLOW_COPYING(ShardType);
    public:

        //
        // schema declarations
        //

        // Name of the shards collection in the config server.
        static const std::string ConfigNS;

        // Field names and types in the shards collection type.
        static const BSONField<std::string> name;
        static const BSONField<std::string> host;
        static const BSONField<bool> draining;
        static const BSONField<long long> maxSize;
        static const BSONField<BSONArray> tags;

        //
        // shards type methods
        //

        ShardType();
        ~ShardType();

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
        void cloneTo(ShardType* other) const;

        /**
         * Returns a string representation of the current internal state.
         */
        std::string toString() const;

        //
        // individual field accessors
        //

        // Mandatory Fields
        void setName(const StringData& name) {
            _name = name.toString();
            _isNameSet = true;
        }

        void unsetName() { _isNameSet = false; }

        bool isNameSet() const { return _isNameSet; }

        // Calling get*() methods when the member is not set results in undefined behavior
        const std::string getName() const {
            dassert(_isNameSet);
            return _name;
        }

        void setHost(const StringData& host) {
            _host = host.toString();
            _isHostSet = true;
        }

        void unsetHost() { _isHostSet = false; }

        bool isHostSet() const { return _isHostSet; }

        // Calling get*() methods when the member is not set results in undefined behavior
        const std::string getHost() const {
            dassert(_isHostSet);
            return _host;
        }

        // Optional Fields
        void setDraining(bool draining) {
            _draining = draining;
            _isDrainingSet = true;
        }

        void unsetDraining() { _isDrainingSet = false; }

        bool isDrainingSet() const {
            return _isDrainingSet || draining.hasDefault();
        }

        // Calling get*() methods when the member is not set and has no default results in undefined
        // behavior
        bool getDraining() const {
            if (_isDrainingSet) {
                return _draining;
            } else {
                dassert(draining.hasDefault());
                return draining.getDefault();
            }
        }
        void setMaxSize(long long maxSize) {
            _maxSize = maxSize;
            _isMaxSizeSet = true;
        }

        void unsetMaxSize() { _isMaxSizeSet = false; }

        bool isMaxSizeSet() const {
            return _isMaxSizeSet || maxSize.hasDefault();
        }

        // Calling get*() methods when the member is not set and has no default results in undefined
        // behavior
        long long getMaxSize() const {
            if (_isMaxSizeSet) {
                return _maxSize;
            } else {
                dassert(maxSize.hasDefault());
                return maxSize.getDefault();
            }
        }
        void setTags(BSONArray tags) {
            _tags = tags;
            _isTagsSet = true;
        }

        void unsetTags() { _isTagsSet = false; }

        bool isTagsSet() const {
            return _isTagsSet || tags.hasDefault();
        }

        // Calling get*() methods when the member is not set and has no default results in undefined
        // behavior
        BSONArray getTags() const {
            if (_isTagsSet) {
                return _tags;
            } else {
                dassert(tags.hasDefault());
                return tags.getDefault();
            }
        }

    private:
        // Convention: (M)andatory, (O)ptional, (S)pecial rule.
        std::string _name;     // (M)  shard's id
        bool _isNameSet;
        std::string _host;     // (M)  connection string for the host(s)
        bool _isHostSet;
        bool _draining;     // (O)  is it draining chunks?
        bool _isDrainingSet;
        long long _maxSize;     // (O)  maximum allowed disk space in MB
        bool _isMaxSizeSet;
        BSONArray _tags;     // (O)  shard tags
        bool _isTagsSet;
    };

} // namespace mongo
