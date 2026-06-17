/**
 *    Copyright (C) 2026-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/query/query_knobs/query_knob_registry.h"

#include "mongo/base/init.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/query/query_knobs/query_knob.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/static_immortal.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <utility>

namespace mongo {
namespace {
// Holds knob entries and the id counter accumulated during static init before QueryKnobRegistryInit
// runs.
struct QueryKnobInitializerContext {
    QueryKnobId::value_t numKnobs = 0;
    std::vector<QueryKnobRegistry::Entry> entries;
} globalQueryKnobInitializerContext;

boost::optional<multiversion::FeatureCompatibilityVersion> extractMinFcv(StringData paramName,
                                                                         const BSONObj& qk) {
    auto fcvElem = qk["fcv"];
    if (fcvElem.eoo()) {
        return boost::none;
    }
    invariant(fcvElem.type() == BSONType::object,
              str::stream() << "query_knob fcv on ServerParameter '" << paramName
                            << "' must be an object, got " << typeName(fcvElem.type()));
    auto minElem = fcvElem.Obj()["min"];
    if (minElem.eoo()) {
        return boost::none;
    }
    invariant(minElem.type() == BSONType::string,
              str::stream() << "query_knob fcv.min on ServerParameter '" << paramName
                            << "' must be a string, got " << typeName(minElem.type()));
    auto str = minElem.valueStringData();
    try {
        return multiversion::parseVersionForFeatureFlags(str);
    } catch (const DBException& ex) {
        invariant(false,
                  str::stream() << "query_knob fcv.min '" << str << "' on ServerParameter '"
                                << paramName << "' is not a valid FCV: " << ex.reason());
    }
    MONGO_UNREACHABLE;
}

}  // namespace

QueryKnobRegistry::Entry::Entry(QueryKnobId id,
                                ServerParameter* param,
                                FromBSONFn fromBSONFn,
                                ToBSONFn toBSONFn,
                                ReadGlobalFn readGlobalFn,
                                AttachOnUpdateFn attachOnUpdateFn)
    : id(id),
      param(param),
      fromBSON(fromBSONFn),
      toBSON(toBSONFn),
      readGlobal(readGlobalFn),
      attachOnUpdate(attachOnUpdateFn) {
    invariant(param, fmt::format("No server parameter found for query knob id {}", id.value));
    const auto qk = param->annotations().getObjectField("query_knob");
    invariant(!qk.isEmpty(),
              str::stream() << "ServerParameter '" << param->name()
                            << "' is missing the query_knob annotation");

    wireName = qk.getStringField("wire_name");
    invariant(!wireName.empty(),
              str::stream() << "ServerParameter '" << param->name()
                            << "': wire name must not be empty");

    // TODO SERVER-125554: once a PQS knob's "minFcv" falls below the binary's supported
    // range the annotation must be dropped, but this invariant then rejects the knob. Need
    // a policy for PQS knobs whose "minFcv" is no longer supported.
    minFcv = extractMinFcv(wireName, qk);

    pqsSettable = qk.hasField("pqs_settable") && qk.getBoolField("pqs_settable");
    invariant(!pqsSettable || minFcv.has_value(),
              str::stream() << "PQS knob '" << param->name() << "' requires minFcv");
}

QueryKnobRegistry::QueryKnobRegistry(std::vector<Entry> entries) : _entries(std::move(entries)) {
    std::sort(
        _entries.begin(), _entries.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    for (size_t i = 0; i < _entries.size(); ++i) {
        const auto& e = _entries[i];
        invariant(e.id.value == i,
                  str::stream() << "QueryKnobRegistry entry id " << e.id.value
                                << " does not match its position " << i);
        if (e.pqsSettable) {
            auto [_, inserted] = _wireNameIndex.try_emplace(std::string{e.wireName}, i);
            invariant(inserted, str::stream() << "duplicate wire name '" << e.wireName << "'");
            ++_knobsExposedToQuerySettingsCount;
        }
    }
}

QueryKnobRegistry& QueryKnobRegistry::_mutableInstance() {
    static StaticImmortal<QueryKnobRegistry> storage{};
    return *storage;
}

const QueryKnobRegistry& QueryKnobRegistry::instance() {
    auto&& inst = _mutableInstance();
    invariant(!inst.entries().empty(),
              "QueryKnobRegistry::instance() called before QueryKnobRegistryInit");
    return inst;
}

void QueryKnobRegistry::init(std::vector<Entry> entries) {
    auto&& inst = _mutableInstance();
    invariant(inst.entries().empty(), "QueryKnobRegistry::init called twice");
    inst = QueryKnobRegistry(std::move(entries));
}

boost::optional<QueryKnobId> QueryKnobRegistry::getKnobIdForName(StringData wireName) const {
    auto it = _wireNameIndex.find(wireName);
    if (it == _wireNameIndex.end()) {
        return boost::none;
    }
    return it->second;
}

const QueryKnobRegistry::Entry& QueryKnobRegistry::entry(QueryKnobId id) const {
    tassert(12317800,
            str::stream() << "QueryKnobRegistry::entry id " << id.value << " out of range (size "
                          << _entries.size() << ")",
            id.value < _entries.size());
    return _entries[id.value];
}

std::span<const QueryKnobRegistry::Entry> QueryKnobRegistry::entries() const {
    return {_entries.cbegin(), _entries.cend()};
}

size_t QueryKnobRegistry::knobCount() const {
    return _entries.size();
}

size_t QueryKnobRegistry::knobsExposedToQuerySettingsCount() const {
    return _knobsExposedToQuerySettingsCount;
}

namespace detail {
void detectOrphanAnnotations(const std::vector<QueryKnobRegistry::Entry>& entries,
                             const ServerParameterSet& params) {
    auto hasEntryFor = [&](StringData name) {
        return std::any_of(entries.begin(), entries.end(), [name](const auto& e) {
            return e.param->name() == name;
        });
    };
    for (auto&& [name, sp] : params.getMap()) {
        if (!sp->annotations().hasField("query_knob")) {
            continue;
        }
        invariant(hasEntryFor(name),
                  str::stream() << "ServerParameter '" << name
                                << "' has a query_knob annotation but no matching QueryKnob<T>");
    }
}

QueryKnobId allocateQueryKnobId() {
    return QueryKnobId(globalQueryKnobInitializerContext.numKnobs++);
}

void registerQueryKnob(QueryKnobRegistry::Entry&& entry) {
    globalQueryKnobInitializerContext.entries.push_back(std::move(entry));
}

namespace {
MONGO_INITIALIZER_WITH_PREREQUISITES(QueryKnobRegistryInit, ("EndServerParameterRegistration"))
(InitializerContext*) {
    auto&& params = ServerParameterSet::getNodeParameterSet();
    auto&& entries = globalQueryKnobInitializerContext.entries;
    detectOrphanAnnotations(entries, *params);
    QueryKnobRegistry::init(std::move(globalQueryKnobInitializerContext.entries));
}

}  // namespace
}  // namespace detail
}  // namespace mongo
