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

#include "mongo/s/type_shard.h"

#include "mongo/s/field_parser.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using mongoutils::str::stream;

    const std::string ShardType::ConfigNS = "config.shards";

    BSONField<std::string> ShardType::name("_id");
    BSONField<std::string> ShardType::host("host");
    BSONField<bool> ShardType::draining("draining");
    BSONField<long long> ShardType::maxSize("maxSize");
    BSONField<BSONArray> ShardType::tags("tags");

    ShardType::ShardType() {
        clear();
    }

    ShardType::~ShardType() {
    }

    bool ShardType::isValid(std::string* errMsg) const {

        string dummy;
        if (!errMsg) errMsg = &dummy;

        // All the mandatory fields must be present.
        if (_name.empty()) {
            *errMsg = stream() << "missing " << name.name() << " field";
            return false;
        }
        if (_host.empty()) {
            *errMsg = stream() << "missing " << host.name() << " field";
            return false;
        }

        return true;
    }

    BSONObj ShardType::toBSON() const {
        BSONObjBuilder builder;
        if (!_name.empty()) builder.append(name(), _name);
        if (!_host.empty()) builder.append(host(), _host);
        if (_draining) builder.append(draining(), _draining);
        if (_maxSize > 0) builder.append(maxSize(), _maxSize);
        if (_tags.nFields()) builder.append(tags(), _tags);
        return builder.obj();
    }

    bool ShardType::parseBSON(BSONObj source, string* errMsg) {
        clear();

        string dummy;
        if (!errMsg) errMsg = &dummy;

        if (!FieldParser::extract(source, name, "", &_name, errMsg)) return false;
        if (!FieldParser::extract(source, host, "", &_host, errMsg)) return false;
        if (!FieldParser::extract(source, draining, false, &_draining, errMsg)) return false;
        if (!FieldParser::extract(source, maxSize, 0LL, &_maxSize, errMsg)) return false;
        if (!FieldParser::extract(source, tags, BSONArray(), &_tags, errMsg)) return false;

        return true;
    }

    void ShardType::clear() {
        _name.clear();
        _host.clear();
        _draining = false;
        _maxSize = 0;
        _tags = BSONArray();
    }

    void ShardType::cloneTo(ShardType* other) const {
        other->clear();
        other->_name = _name;
        other->_host = _host;
        other->_draining = _draining;
        other->_maxSize = _maxSize;
        other->_tags = _tags;
    }

    std::string ShardType::toString() const {
        return toBSON().toString();
    }

} // namespace mongo
