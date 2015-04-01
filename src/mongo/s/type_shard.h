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

#include <boost/optional.hpp>
#include <string>
#include <vector>

#include "mongo/bson/bson_field.h"

namespace mongo {

    struct BSONArray;
    class BSONObj;
    class Status;
    template<typename T> class StatusWith;

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
     *     StatusWith<ShardType> exampleResult = ShardType::fromBSON(exampleDoc);
     *     if (!exampleResult.isOK()) {
     *         // handle error -- exampleResult.getStatus()
     *     }
     *     ShardType exampleType = exampleResult.getValue();
     *     // use 'exampleType'
     *
     */
    class ShardType {
    public:

        // Name of the shards collection in the config server.
        static const std::string ConfigNS;

        // Field names and types in the shards collection type.
        static const BSONField<std::string> name;
        static const BSONField<std::string> host;
        static const BSONField<bool> draining;
        static const BSONField<long long> maxSize;
        static const BSONField<BSONArray> tags;

        ShardType();
        ~ShardType();

        /**
         * Constructs a new ShardType object from BSON.
         * Also does validation of the contents.
         */
        static StatusWith<ShardType> fromBSON(const BSONObj& source);

        /**
         * Returns OK if all fields have been set. Otherwise returns NoSuchKey
         * and information about the first field that is missing.
         */
        Status validate() const;

        /**
         * Returns the BSON representation of the entry.
         */
        BSONObj toBSON() const;

        /**
         * Clears the internal state.
         */
        void clear();

        /**
         * Copies all the fields present in 'this' to 'other'.
         */
        void cloneTo(ShardType* other) const;

        /**
         * Returns a std::string representation of the current internal state.
         */
        std::string toString() const;

        bool isNameSet() const { return _name.is_initialized(); }
        const std::string& getName() const { return _name.get(); }
        void setName(const std::string& name);

        bool isHostSet() const { return _host.is_initialized(); }
        const std::string& getHost() const { return _host.get(); }
        void setHost(const std::string& host);

        bool isDrainingSet() const { return _draining.is_initialized(); }
        const bool getDraining() const { return _draining.get(); }
        void setDraining(const bool draining);

        bool isMaxSizeSet() const { return _maxSize.is_initialized(); }
        const long long getMaxSize() const { return _maxSize.get(); }
        void setMaxSize(const long long maxSize);

        bool isTagsSet() const { return _tags.is_initialized(); }
        const std::vector<std::string>& getTags() const { return _tags.get(); }
        void setTags(const std::vector<std::string>& tags);

    private:
        // Convention: (M)andatory, (O)ptional, (S)pecial rule.

        // (M)  shard's id
        boost::optional<std::string> _name;
        // (M)  connection string for the host(s)
        boost::optional<std::string> _host;
        // (O) is it draining drunks?
        boost::optional<bool> _draining;
        // (O) maximum allowed disk space in MB
        boost::optional<long long> _maxSize;
        // (O) shard tags
        boost::optional<std::vector<std::string>> _tags;
    };

} // namespace mongo
