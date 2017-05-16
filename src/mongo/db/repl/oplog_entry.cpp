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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/oplog_entry.h"

#include "mongo/db/namespace_string.h"

namespace mongo {
namespace repl {

const int OplogEntry::kOplogVersion = 2;

OplogEntry::OplogEntry(BSONObj rawInput) : raw(std::move(rawInput)) {
    if (MONGO_unlikely(!raw.isOwned())) {
        raw = raw.copy();
    }

    BSONElement version;
    for (auto elem : raw) {
        const auto name = elem.fieldNameStringData();
        if (name == "ns") {
            _ns = NamespaceString(elem.valuestrsafe());
        } else if (name == "op") {
            _opType = elem.valuestrsafe();
        } else if (name == "o2") {
            _o2 = elem.Obj();
        } else if (name == "ts") {
            _ts = elem.timestamp();
        } else if (name == "v") {
            version = elem;
        } else if (name == "o") {
            _o = elem.Obj();
        }
    }

    _version = version.eoo() ? 1 : version.numberInt();
}

bool OplogEntry::isCommand() const {
    return getOpType()[0] == 'c';
}

bool OplogEntry::isCrudOpType() const {
    switch (getOpType()[0]) {
        case 'd':
        case 'i':
        case 'u':
            return getOpType()[1] == 0;
    }
    return false;
}

bool OplogEntry::hasNamespace() const {
    return !getNamespace().isEmpty();
}

int OplogEntry::getVersion() const {
    return _version;
}

BSONElement OplogEntry::getIdElement() const {
    invariant(isCrudOpType());
    switch (getOpType()[0]) {
        case 'u':
            return getObject2()["_id"];
        case 'd':
        case 'i':
            return getObject()["_id"];
    }
    MONGO_UNREACHABLE;
}

OpTime OplogEntry::getOpTime() const {
    return fassertStatusOK(34436, OpTime::parseFromOplogEntry(raw));
}

StringData OplogEntry::getCollectionName() const {
    return getNamespace().coll();
}

std::string OplogEntry::toString() const {
    return raw.toString();
}

std::ostream& operator<<(std::ostream& s, const OplogEntry& o) {
    return s << o.toString();
}

}  // namespace repl
}  // namespace mongo
