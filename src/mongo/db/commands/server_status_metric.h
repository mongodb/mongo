// server_status_metric.h

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

#pragma once

#include <string>

#include "mongo/db/jsobj.h"

namespace mongo {

class ServerStatusMetric {
public:
    /**
     * @param name is a dotted path of a counter name
     *             if name starts with . its treated as a path from the serverStatus root
     *             otherwise it will live under the "counters" namespace
     *             so foo.bar would be serverStatus().counters.foo.bar
     */
    ServerStatusMetric(const std::string& name);
    virtual ~ServerStatusMetric() {}

    std::string getMetricName() const {
        return _name;
    }

    virtual void appendAtLeaf(BSONObjBuilder& b) const = 0;

protected:
    static std::string _parseLeafName(const std::string& name);

    const std::string _name;
    const std::string _leafName;
};

/**
 * usage
 *
 * declared once
 *    Counter counter;
 *    ServerStatusMetricField myAwesomeCounterDisplay( "path.to.counter", &counter );
 *
 * call
 *    counter.hit();
 *
 * will show up in db.serverStatus().metrics.path.to.counter
 */
template <typename T>
class ServerStatusMetricField : public ServerStatusMetric {
public:
    ServerStatusMetricField(const std::string& name, const T* t)
        : ServerStatusMetric(name), _t(t) {}

    const T* get() {
        return _t;
    }

    virtual void appendAtLeaf(BSONObjBuilder& b) const {
        b.append(_leafName, *_t);
    }

private:
    const T* _t;
};
}
