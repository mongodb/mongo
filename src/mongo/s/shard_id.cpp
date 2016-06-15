/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include <functional>
#include <string.h>

#include "mongo/base/status_with.h"
#include "mongo/s/shard_id.h"

namespace mongo {

using std::string;
using std::ostream;

bool ShardId::operator==(const ShardId& other) const {
    return (this->_shardId == other._shardId);
}

bool ShardId::operator!=(const ShardId& other) const {
    return !(*this == other);
}

bool ShardId::operator==(const string& other) const {
    return (this->_shardId == other);
}

bool ShardId::operator!=(const string& other) const {
    return !(*this == other);
}

ShardId::operator StringData() {
    return StringData(_shardId.data(), _shardId.size());
}

const string& ShardId::toString() const {
    return _shardId;
}

bool ShardId::isValid() const {
    return !_shardId.empty();
}

ostream& operator<<(ostream& os, const ShardId& shardId) {
    os << shardId._shardId;
    return os;
}

bool ShardId::operator<(const ShardId& other) const {
    return _shardId < other._shardId;
}

int ShardId::compare(const ShardId& other) const {
    return _shardId.compare(other._shardId);
}

std::size_t ShardId::Hasher::operator()(const ShardId& shardId) const {
    return std::hash<std::string>()(shardId._shardId);
}
}  // namespace mongo
