/**
 *    Copyright (C) 2012-2015 MongoDB Inc.
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

#include <boost/optional.hpp>
#include <string>

#include "mongo/db/jsobj.h"
#include "mongo/s/chunk_version.h"

namespace mongo {

class BSONObj;
class Status;
template <typename T>
class StatusWith;

/**
 * This class represents the layout and contents of documents contained in the
 * config.chunks collection. All manipulation of documents coming from that
 * collection should be done with this class.
 */
class ChunkType {
public:
    // Name of the chunks collection in the config server.
    static const std::string ConfigNS;

    // Field names and types in the chunks collection type.
    static const BSONField<std::string> name;
    static const BSONField<std::string> ns;
    static const BSONField<BSONObj> min;
    static const BSONField<BSONObj> max;
    static const BSONField<BSONArray> version;
    static const BSONField<std::string> shard;
    static const BSONField<bool> jumbo;
    static const BSONField<Date_t> DEPRECATED_lastmod;
    static const BSONField<OID> DEPRECATED_epoch;


    /**
     * Constructs a new ChunkType object from BSON.
     * Also does validation of the contents.
     */
    static StatusWith<ChunkType> fromBSON(const BSONObj& source);

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
     * Returns a std::string representation of the current internal state.
     */
    std::string toString() const;

    const std::string& getName() const {
        return _name.get();
    }
    void setName(const std::string& name);

    const std::string& getNS() const {
        return _ns.get();
    }
    void setNS(const std::string& name);

    const BSONObj& getMin() const {
        return _min.get();
    }
    void setMin(const BSONObj& min);

    const BSONObj& getMax() const {
        return _max.get();
    }
    void setMax(const BSONObj& max);

    bool isVersionSet() const {
        return _version.is_initialized();
    }
    const ChunkVersion& getVersion() const {
        return _version.get();
    }
    void setVersion(const ChunkVersion& version);

    const std::string& getShard() const {
        return _shard.get();
    }
    void setShard(const std::string& shard);

    bool getJumbo() const {
        return _jumbo.get_value_or(false);
    }
    void setJumbo(bool jumbo);

private:
    // Convention: (M)andatory, (O)ptional, (S)pecial rule.

    // (M)  chunk's id
    boost::optional<std::string> _name;
    // (M)  collection this chunk is in
    boost::optional<std::string> _ns;
    // (M)  first key of the range, inclusive
    boost::optional<BSONObj> _min;
    // (M)  last key of the range, non-inclusive
    boost::optional<BSONObj> _max;
    // (M)  version of this chunk
    boost::optional<ChunkVersion> _version;
    // (M)  shard this chunk lives in
    boost::optional<std::string> _shard;
    // (O)  too big to move?
    boost::optional<bool> _jumbo;
};

}  // namespace mongo
