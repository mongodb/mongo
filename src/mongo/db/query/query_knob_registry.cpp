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

#include "mongo/db/query/query_knob_registry.h"

#include "mongo/base/init.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/query/query_knob.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/static_immortal.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <utility>

namespace mongo {
namespace {

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

QueryKnobRegistry::QueryKnobRegistry(std::vector<Entry> entries) : _entries(std::move(entries)) {
    for (size_t i = 0; i < _entries.size(); ++i) {
        const auto& e = _entries[i];
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
    auto& inst = _mutableInstance();
    invariant(inst._initialized,
              "QueryKnobRegistry::instance() called before QueryKnobRegistryInit");
    return inst;
}

void QueryKnobRegistry::init(QueryKnobRegistry reg) {
    auto& inst = _mutableInstance();
    invariant(!inst._initialized, "QueryKnobRegistry::init called twice");
    inst = std::move(reg);
    inst._writeBackDescriptorIndexes();
    inst._initialized = true;
}

void QueryKnobRegistry::_writeBackDescriptorIndexes() {
    for (size_t i = 0; i < _entries.size(); ++i) {
        _entries[i].knob.index = i;
    }
}

boost::optional<size_t> QueryKnobRegistry::getKnobIdForName(StringData wireName) const {
    auto it = _wireNameIndex.find(wireName);
    if (it == _wireNameIndex.end()) {
        return boost::none;
    }
    return it->second;
}

const QueryKnobRegistry::Entry& QueryKnobRegistry::entry(size_t id) const {
    tassert(12317800,
            str::stream() << "QueryKnobRegistry::entry id " << id << " out of range (size "
                          << _entries.size() << ")",
            id < _entries.size());
    return _entries[id];
}

size_t QueryKnobRegistry::knobCount() const {
    return _entries.size();
}

size_t QueryKnobRegistry::knobsExposedToQuerySettingsCount() const {
    return _knobsExposedToQuerySettingsCount;
}

namespace detail {

void QueryKnobRegistryBuilder::addFromServerParameter(QueryKnobRegistry::Entry entry) {
    invariant(!entry.wireName.empty(), "wire name must not be empty");
    if (entry.pqsSettable) {
        // TODO SERVER-125554: once a PQS knob's "minFcv" falls below the binary's supported
        // range the annotation must be dropped, but this invariant then rejects the knob. Need
        // a policy for PQS knobs whose "minFcv" is no longer supported.
        invariant(entry.minFcv.has_value(),
                  str::stream() << "PQS knob '" << entry.wireName << "' requires minFcv");
    }
    _entries.push_back(std::move(entry));
}

void QueryKnobRegistryBuilder::addFromServerParameter(QueryKnobBase& knob, ServerParameter* param) {
    invariant(param,
              str::stream() << "no ServerParameter for QueryKnob '" << knob.paramName << "'");
    addFromServerParameter(knob, *param, param->annotations().getObjectField("query_knob"));
}

void QueryKnobRegistryBuilder::addFromServerParameter(QueryKnobBase& knob,
                                                      ServerParameter& param,
                                                      const BSONObj& queryKnobAnnotation) {
    invariant(!queryKnobAnnotation.isEmpty(),
              str::stream() << "ServerParameter '" << knob.paramName
                            << "' is missing the query_knob annotation");
    addFromServerParameter(QueryKnobRegistry::Entry{
        .knob = knob,
        .param = param,
        .wireName = queryKnobAnnotation.getStringField("wire_name"),
        .pqsSettable = queryKnobAnnotation.hasField("pqs_settable") &&
            queryKnobAnnotation.getBoolField("pqs_settable"),
        .minFcv = extractMinFcv(knob.paramName, queryKnobAnnotation),
    });
}

QueryKnobRegistry QueryKnobRegistryBuilder::build() && {
    return QueryKnobRegistry{std::move(_entries)};
}

void QueryKnobRegistryBuilder::detectOrphanAnnotations(const ServerParameterSet& params) const {
    auto hasEntryFor = [this](StringData name) {
        return std::any_of(_entries.begin(), _entries.end(), [name](const auto& e) {
            return e.knob.paramName == name;
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

}  // namespace detail

MONGO_INITIALIZER_WITH_PREREQUISITES(QueryKnobRegistryInit, ("EndServerParameterRegistration"))
(InitializerContext*) {
    auto&& params = ServerParameterSet::getNodeParameterSet();
    detail::QueryKnobRegistryBuilder builder;
    for (auto* knob : QueryKnobDescriptorSet::get().knobs()) {
        builder.addFromServerParameter(*knob, params->getIfExists(knob->paramName));
    }
    builder.detectOrphanAnnotations(*params);
    QueryKnobRegistry::init(std::move(builder).build());
}

}  // namespace mongo
