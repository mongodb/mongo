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

#include "mongo/db/commands/server_status_internal.h"

#include <iostream>

#include "mongo/bson/bsontypes.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/util/str.h"

namespace mongo {

using std::cerr;
using std::endl;
using std::map;
using std::string;

MetricTree* MetricTree::theMetricTree = nullptr;

void MetricTree::add(ServerStatusMetric* metric) {
    string name = metric->getMetricName();
    if (name[0] == '.')
        _add(name.substr(1), metric);
    else
        _add(str::stream() << "metrics." << name, metric);
}

void MetricTree::_add(const string& path, ServerStatusMetric* metric) {
    size_t idx = path.find(".");
    if (idx == string::npos) {
        if (_subtrees.count(path) > 0) {
            cerr << "metric conflict on: " << path << endl;
            fassertFailed(6483100);
        }
        if (_metrics.count(path) > 0) {
            cerr << "duplicate metric: " << path << endl;
            fassertFailed(6483101);
        }
        _metrics[path] = metric;
        return;
    }

    string myLevel = path.substr(0, idx);
    if (_metrics.count(myLevel) > 0) {
        cerr << "metric conflict on: " << path << endl;
        fassertFailed(16461);
    }

    MetricTree*& sub = _subtrees[myLevel];
    if (!sub)
        sub = new MetricTree();
    sub->_add(path.substr(idx + 1), metric);
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
        bb.done();
    }
}

}  // namespace mongo
