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

#include <cstring> // for strcmp
#include "mongo/s/type_chunk.h"

#include "mongo/s/field_parser.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using mongoutils::str::stream;

    const std::string ChunkType::ConfigNS = "config.chunks";

    BSONField<std::string> ChunkType::name("_id");
    BSONField<std::string> ChunkType::ns("ns");
    BSONField<BSONObj> ChunkType::min("min");
    BSONField<BSONObj> ChunkType::max("max");
    BSONField<BSONArray> ChunkType::version("version");
    BSONField<std::string> ChunkType::shard("shard");
    BSONField<bool> ChunkType::jumbo("jumbo");

    BSONField<Date_t> ChunkType::DEPRECATED_lastmod("lastmod");
    BSONField<OID> ChunkType::DEPRECATED_epoch("lastmodEpoch");

    ChunkType::ChunkType() {
        clear();
    }

    ChunkType::~ChunkType() {
    }

    bool ChunkType::isValid(std::string* errMsg) const {
        std::string dummy;

        if (errMsg == NULL) errMsg = &dummy;

        // All the mandatory fields must be present.
        if (_name.empty()) {
            *errMsg = stream() << "missing " << name.name() << " field";
            return false;
        }
        if (_ns.empty()) {
            *errMsg = stream() << "missing " << ns.name() << " field";
            return false;
        }
        if (!_min.nFields()) {
            *errMsg = stream() << "missing " << min.name() << " field";
            return false;
        }
        if (!_max.nFields()) {
            *errMsg = stream() << "missing " << max.name() << " field";
            return false;
        }
        if (_version._combined == 0ULL) {
            *errMsg = stream() << "missing " << version.name() << " field";
            return false;
        }
        if (_shard.empty()) {
            *errMsg = stream() << "missing " << shard.name() << " field";
            return false;
        }

        // 'min' and 'max' must share the same fields.
        if (_min.nFields() != _max.nFields()) {
            *errMsg = stream() << "min and max have a different number of keys";
            return false;
        }
        BSONObjIterator minIt(_min);
        BSONObjIterator maxIt(_max);
        while (minIt.more() && maxIt.more()) {
            BSONElement minElem = minIt.next();
            BSONElement maxElem = maxIt.next();
            if (strcmp(minElem.fieldName(), maxElem.fieldName())) {
                *errMsg = stream() << "min and max must have the same set of keys";
                return false;
            }
        }

        // 'max' should be greater than 'min'.
        if (_min.woCompare(_max) >= 0) {
            *errMsg = stream() << "max key must be greater than min key";
            return false;
        }

        return true;
    }

    BSONObj ChunkType::toBSON() const {
        BSONObjBuilder builder;

        if (!_name.empty()) builder.append(name(), _name);
        if (!_ns.empty()) builder.append(ns(), _ns);
        if (_min.nFields()) builder.append(min(), _min);
        if (_max.nFields()) builder.append(max(), _max);

        // For now, write both the deprecated *and* the new fields

        _version.addToBSON(builder, version());
        _version.addToBSON(builder, DEPRECATED_lastmod());

        if (!_shard.empty()) builder.append(shard(), _shard);
        if (_jumbo) builder.append(jumbo(), _jumbo);

        return builder.obj();
    }

    string ChunkType::toString() const {
        return toBSON().toString();
    }

    bool ChunkType::parseBSON(BSONObj source, string* errMsg) {
        clear();

        string dummy;
        if (!errMsg) errMsg = &dummy;

        if (!FieldParser::extract(source, name, "", &_name, errMsg)) return false;
        if (!FieldParser::extract(source, ns, "", &_ns, errMsg)) return false;
        if (!FieldParser::extract(source, min, BSONObj(), &_min, errMsg)) return false;
        if (!FieldParser::extract(source, max, BSONObj(), &_max, errMsg)) return false;
        if (!FieldParser::extract(source, shard, "", &_shard, errMsg)) return false;
        if (!FieldParser::extract(source, jumbo, false, &_jumbo, errMsg)) return false;

        //
        // ChunkVersion backward compatibility logic contained in ChunkVersion
        //

        // ChunkVersion is currently encoded as { 'version': [<TS>,<OID>] }

        if (ChunkVersion::canParseBSON(source, version())) {
            _version = ChunkVersion::fromBSON(source, version());
        }
        else if (ChunkVersion::canParseBSON(source, DEPRECATED_lastmod())) {
            _version = ChunkVersion::fromBSON(source, DEPRECATED_lastmod());
        }

        return true;
    }

    void ChunkType::clear() {
        _name.clear();
        _ns.clear();
        _min = BSONObj();
        _max = BSONObj();
        _version = ChunkVersion();
        _shard.clear();
        _jumbo = false;
    }

    void ChunkType::cloneTo(ChunkType* other) const {
        other->clear();
        other->_name = _name;
        other->_ns = _ns;
        other->_min = _min;
        other->_max = _max;
        other->_version = _version;
        other->_shard = _shard;
        other->_jumbo = _jumbo;
    }

} // namespace mongo
