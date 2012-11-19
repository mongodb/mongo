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
        if (errMsg == NULL) {
            errMsg = &dummy;
        }

        // All the mandatory fields must be present.
        if (_name.empty()) {
            *errMsg = stream() << "missing " << name.name() << " field";
            return false;
        }
        if (_ns.empty()) {
            *errMsg = stream() << "missing " << ns.name() << " field";
            return false;
        }
        if (! _min.nFields()) {
            *errMsg = stream() << "missing " << min.name() << " field";
            return false;
        }
        if (! _max.nFields()) {
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

        if (_version._combined != 0ULL) {
            BSONArrayBuilder arrBuilder(builder.subarrayStart(version()));
            arrBuilder.appendTimestamp(_version._major, _version._minor);
            arrBuilder.append(_version._epoch);
            arrBuilder.done();
        }

        if (!_shard.empty()) builder.append(shard(), _shard);
        if (_jumbo) builder.append(jumbo(), _jumbo);

        return builder.obj();
    }

    void ChunkType::parseBSON(BSONObj source) {
        clear();

        bool ok = true;
        ok &= FieldParser::extract(source, name, "", &_name);
        ok &= FieldParser::extract(source, ns, "", &_ns);
        ok &= FieldParser::extract(source, min, BSONObj(), &_min);
        ok &= FieldParser::extract(source, max, BSONObj(), &_max);
        ok &= FieldParser::extract(source, shard, "", &_shard);
        ok &= FieldParser::extract(source, jumbo, false, &_jumbo);
        if (! ok) {
            clear();
            return;
        }

        //
        // ShardChunkVersion backward compatibility logic
        //

        // ShardChunkVersion is currently encoded as { 'version': [<TS>,<OID>] }
        BSONArray arrVersion;
        ok = FieldParser::extract(source, version, BSONArray(), &arrVersion);
        if (! ok) {
            clear();
            return;
        }
        else if (arrVersion.nFields()) {
            bool okVersion;
            _version = ShardChunkVersion::fromBSON(arrVersion, &okVersion);
            if (! okVersion) {
                clear();
            }
            return;
        }

        // If we haven't found the current format try parsing the deprecated format
        // { lastmod: <TS>, lastmodEpoch: <OID> }.
        Date_t lastmod;
        OID epoch;
        ok = FieldParser::extract(source, DEPRECATED_lastmod, time(0), &lastmod);
        ok &= FieldParser::extract(source, DEPRECATED_epoch, OID(), &epoch);
        if (! ok) {
            clear();
        }
        else {
            _version = ShardChunkVersion(lastmod.millis, epoch);
        }
    }

    void ChunkType::clear() {
        _name.clear();
        _ns.clear();
        _min = BSONObj();
        _max = BSONObj();
        _version = ShardChunkVersion();
        _shard.clear();
        _jumbo = false;
    }

    void ChunkType::cloneTo(ChunkType* other) {
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
