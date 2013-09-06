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
#include "mongo/s/type_chunk.h"

#include <cstring>

#include "mongo/db/field_parser.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using mongoutils::str::stream;

    const std::string ChunkType::ConfigNS = "config.chunks";

    const BSONField<std::string> ChunkType::name("_id");
    const BSONField<std::string> ChunkType::ns("ns");
    const BSONField<BSONObj> ChunkType::min("min");
    const BSONField<BSONObj> ChunkType::max("max");
    const BSONField<BSONArray> ChunkType::version("version");
    const BSONField<std::string> ChunkType::shard("shard");
    const BSONField<bool> ChunkType::jumbo("jumbo");
    const BSONField<Date_t> ChunkType::DEPRECATED_lastmod("lastmod");
    const BSONField<OID> ChunkType::DEPRECATED_epoch("lastmodEpoch");

    ChunkType::ChunkType() {
        clear();
    }

    ChunkType::~ChunkType() {
    }

    bool ChunkType::isValid(std::string* errMsg) const {
        std::string dummy;
        if (errMsg == NULL) {
            errMsg = &dummy;
        }

        // All the mandatory fields must be present.
        if (!_isNameSet) {
            *errMsg = stream() << "missing " << name.name() << " field";
            return false;
        }
        if (!_isNsSet) {
            *errMsg = stream() << "missing " << ns.name() << " field";
            return false;
        }
        if (!_isMinSet) {
            *errMsg = stream() << "missing " << min.name() << " field";
            return false;
        }
        if (!_isMaxSet) {
            *errMsg = stream() << "missing " << max.name() << " field";
            return false;
        }
        if (!_isVersionSet) {
            *errMsg = stream() << "missing " << version.name() << " field";
            return false;
        }
        if (!_isShardSet) {
            *errMsg = stream() << "missing " << shard.name() << " field";
            return false;
        }

        // NOTE: all the following semantic checks should eventually become the caller's
        // responsibility, and should be moved out of this class completely

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

        if (_isNameSet) builder.append(name(), _name);
        if (_isNsSet) builder.append(ns(), _ns);
        if (_isMinSet) builder.append(min(), _min);
        if (_isMaxSet) builder.append(max(), _max);

        // For now, write both the deprecated *and* the new fields
        if (_isVersionSet) {
            _version.addToBSON(builder, version());
            _version.addToBSON(builder, DEPRECATED_lastmod());
        }

        if (_isShardSet) builder.append(shard(), _shard);
        if (_isJumboSet) builder.append(jumbo(), _jumbo);

        return builder.obj();
    }

    bool ChunkType::parseBSON(const BSONObj& source, string* errMsg) {
        clear();

        std::string dummy;
        if (!errMsg) errMsg = &dummy;

        FieldParser::FieldState fieldState;
        fieldState = FieldParser::extract(source, name, &_name, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isNameSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, ns, &_ns, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isNsSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, min, &_min, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isMinSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, max, &_max, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isMaxSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, shard, &_shard, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isShardSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, jumbo, &_jumbo, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isJumboSet = fieldState == FieldParser::FIELD_SET;

        //
        // ChunkVersion backward compatibility logic contained in ChunkVersion
        //

        // ChunkVersion is currently encoded as { 'version': [<TS>,<OID>] }

        if (ChunkVersion::canParseBSON(source, version())) {
            _version = ChunkVersion::fromBSON(source, version());
            _isVersionSet = true;
        }
        else if (ChunkVersion::canParseBSON(source, DEPRECATED_lastmod())) {
            _version = ChunkVersion::fromBSON(source, DEPRECATED_lastmod());
            _isVersionSet = true;
        }

        return true;
    }

    void ChunkType::clear() {

        _name.clear();
        _isNameSet = false;

        _ns.clear();
        _isNsSet = false;

        _min = BSONObj();
        _isMinSet = false;

        _max = BSONObj();
        _isMaxSet = false;

        _version = ChunkVersion();
        _isVersionSet = false;

        _shard.clear();
        _isShardSet = false;

        _jumbo = false;
        _isJumboSet = false;

    }

    void ChunkType::cloneTo(ChunkType* other) const {
        other->clear();

        other->_name = _name;
        other->_isNameSet = _isNameSet;

        other->_ns = _ns;
        other->_isNsSet = _isNsSet;

        other->_min = _min;
        other->_isMinSet = _isMinSet;

        other->_max = _max;
        other->_isMaxSet = _isMaxSet;

        other->_version = _version;
        other->_isVersionSet = _isVersionSet;

        other->_shard = _shard;
        other->_isShardSet = _isShardSet;

        other->_jumbo = _jumbo;
        other->_isJumboSet = _isJumboSet;

    }

    std::string ChunkType::toString() const {
        return toBSON().toString();
    }

} // namespace mongo
