// server_status_internal.cpp

/**
*    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/db/commands/server_status_internal.h"

#include <iostream>

#include "mongo/db/commands/server_status_metric.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::cerr;
using std::endl;
using std::map;
using std::string;

using namespace mongoutils;

MetricTree* MetricTree::theMetricTree = NULL;

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
        _metrics[path] = metric;
        return;
    }

    string myLevel = path.substr(0, idx);
    if (_metrics.count(myLevel) > 0) {
        cerr << "metric conflict on: " << myLevel << endl;
        fassertFailed(16461);
    }

    MetricTree*& sub = _subtrees[myLevel];
    if (!sub)
        sub = new MetricTree();
    sub->_add(path.substr(idx + 1), metric);
}

void MetricTree::appendTo(BSONObjBuilder& b) const {
    for (map<string, ServerStatusMetric*>::const_iterator i = _metrics.begin(); i != _metrics.end();
         ++i) {
        i->second->appendAtLeaf(b);
    }

    for (map<string, MetricTree*>::const_iterator i = _subtrees.begin(); i != _subtrees.end();
         ++i) {
        BSONObjBuilder bb(b.subobjStart(i->first));
        i->second->appendTo(bb);
        bb.done();
    }
}
}
