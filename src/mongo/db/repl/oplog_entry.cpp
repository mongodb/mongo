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

    for (auto elem : raw) {
        const auto name = elem.fieldNameStringData();
        if (name == "ns") {
            ns = elem.valuestrsafe();
        } else if (name == "op") {
            opType = elem.valuestrsafe();
        } else if (name == "o2") {
            o2 = elem;
        } else if (name == "ts") {
            ts = elem;
        } else if (name == "v") {
            version = elem;
        } else if (name == "o") {
            o = elem;
        }
    }
}

bool OplogEntry::isCommand() const {
    return opType[0] == 'c';
}

bool OplogEntry::isCrudOpType() const {
    switch (opType[0]) {
        case 'd':
        case 'i':
        case 'u':
            return opType[1] == 0;
    }
    return false;
}

bool OplogEntry::hasNamespace() const {
    return !ns.empty();
}

int OplogEntry::getVersion() const {
    return version.eoo() ? 1 : version.Int();
}

BSONElement OplogEntry::getIdElement() const {
    invariant(isCrudOpType());
    switch (opType[0]) {
        case 'u':
            return o2.Obj()["_id"];
        case 'd':
        case 'i':
            return o.Obj()["_id"];
    }
    MONGO_UNREACHABLE;
}

OpTime OplogEntry::getOpTime() const {
    return fassertStatusOK(34436, OpTime::parseFromOplogEntry(raw));
}

Seconds OplogEntry::getTimestampSecs() const {
    return Seconds(ts.timestamp().getSecs());
}

StringData OplogEntry::getCollectionName() const {
    return nsToCollectionSubstring(ns);
}

std::string OplogEntry::toString() const {
    return raw.toString();
}

std::ostream& operator<<(std::ostream& s, const OplogEntry& o) {
    return s << o.toString();
}

}  // namespace repl
}  // namespace mongo
