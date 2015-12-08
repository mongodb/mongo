// progress_meter.cpp

/*    Copyright 2009 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"
#undef MONGO_PCH_WHITELISTED  // needed for log.h

#include "mongo/util/progress_meter.h"

#include "mongo/util/log.h"

using namespace std;

namespace mongo {

void ProgressMeter::reset(unsigned long long total, int secondsBetween, int checkInterval) {
    _total = total;
    _secondsBetween = secondsBetween;
    _checkInterval = checkInterval;

    _done = 0;
    _hits = 0;
    _lastTime = (int)time(0);

    _active = true;
}


bool ProgressMeter::hit(int n) {
    if (!_active) {
        warning() << "hit an inactive ProgressMeter" << endl;
        return false;
    }

    _done += n;
    _hits++;
    if (_hits % _checkInterval)
        return false;

    int t = (int)time(0);
    if (t - _lastTime < _secondsBetween)
        return false;

    if (_total > 0) {
        int per = (int)(((double)_done * 100.0) / (double)_total);
        LogstreamBuilder out = log();
        out << "  " << _name << ": " << _done;

        if (_showTotal) {
            out << '/' << _total << ' ' << per << '%';
        }

        if (!_units.empty()) {
            out << " (" << _units << ")";
        }
        out << endl;
    }
    _lastTime = t;
    return true;
}

string ProgressMeter::toString() const {
    if (!_active)
        return "";
    stringstream buf;
    buf << _name << ": " << _done << '/' << _total << ' ' << (_done * 100) / _total << '%';

    if (!_units.empty()) {
        buf << " (" << _units << ")" << endl;
    }

    return buf.str();
}
}
