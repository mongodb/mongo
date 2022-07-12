/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/commands/server_status_metric.h"

#include <fmt/format.h>
#include <memory>

#include "mongo/bson/bsontypes.h"
#include "mongo/logv2/log.h"
#include "mongo/util/static_immortal.h"
#include "mongo/util/str.h"
#include "mongo/util/synchronized_value.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {

using namespace fmt::literals;

ServerStatusMetric::ServerStatusMetric(const std::string& nameIn)
    : _name(nameIn), _leafName(_parseLeafName(nameIn)) {}

std::string ServerStatusMetric::_parseLeafName(const std::string& name) {
    size_t idx = name.rfind(".");
    if (idx == std::string::npos)
        return name;
    return name.substr(idx + 1);
}

MetricTree* globalMetricTree(bool create) {
    static StaticImmortal<synchronized_value<std::unique_ptr<MetricTree>>> instance{};
    auto updateGuard = **instance;
    if (create && !*updateGuard)
        *updateGuard = std::make_unique<MetricTree>();
    return updateGuard->get();
}

void MetricTree::add(std::unique_ptr<ServerStatusMetric> metric) {
    std::string name = metric->getMetricName();
    if (!name.empty()) {
        if (auto begin = name.begin(); *begin == '.')
            name.erase(begin);
        else
            name = "metrics.{}"_format(name);
    }

    if (!name.empty())
        _add(name, std::move(metric));
}

void MetricTree::_add(const std::string& path, std::unique_ptr<ServerStatusMetric> metric) {
    size_t idx = path.find('.');
    if (idx == std::string::npos) {
        if (_subtrees.count(path) > 0)
            LOGV2_FATAL(6483100, "metric conflict", "path"_attr = path);

        if (!_metrics.try_emplace(path, std::move(metric)).second)
            LOGV2_FATAL(6483102, "duplicate metric", "path"_attr = path);

        return;
    }

    std::string myLevel = path.substr(0, idx);
    if (_metrics.count(myLevel) > 0)
        LOGV2_FATAL(16461, "metric conflict", "path"_attr = path);

    auto& sub = _subtrees[myLevel];
    if (!sub)
        sub = std::make_unique<MetricTree>();
    sub->_add(path.substr(idx + 1), std::move(metric));
}

void MetricTree::appendTo(BSONObjBuilder& b) const {
    for (const auto& i : _metrics) {
        i.second->appendAtLeaf(b);
    }

    for (const auto& i : _subtrees) {
        BSONObjBuilder bb(b.subobjStart(i.first));
        i.second->appendTo(bb);
        bb.done();
    }
}

void MetricTree::appendTo(const BSONObj& excludePaths, BSONObjBuilder& b) const {
    auto fieldNamesInExclude = excludePaths.getFieldNames<stdx::unordered_set<std::string>>();
    for (const auto& i : _metrics) {
        auto key = i.first;
        auto el = fieldNamesInExclude.contains(key) ? excludePaths.getField(key) : BSONElement();
        if (el) {
            uassert(ErrorCodes::InvalidBSONType,
                    "Exclusion value for a leaf must be a boolean.",
                    el.type() == Bool);
            if (el.boolean() == false) {
                continue;
            }
        }
        i.second->appendAtLeaf(b);
    }

    for (const auto& i : _subtrees) {
        auto key = i.first;
        auto el = fieldNamesInExclude.contains(key) ? excludePaths.getField(key) : BSONElement();
        if (el) {
            uassert(ErrorCodes::InvalidBSONType,
                    "Exclusion value must be a boolean or a nested object.",
                    el.type() == Bool || el.type() == Object);
            if (el.isBoolean() && el.boolean() == false) {
                continue;
            }
        }

        BSONObjBuilder bb(b.subobjStart(key));
        if (el.type() == Object) {
            i.second->appendTo(el.embeddedObject(), bb);
        } else {
            i.second->appendTo(bb);
        }
    }
}

}  // namespace mongo
