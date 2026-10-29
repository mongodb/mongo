// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/util/progress_meter.h"

#include "mongo/logv2/attribute_storage.h"
#include "mongo/logv2/log.h"

#include <ctime>
#include <ostream>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {

void ProgressMeter::reset(unsigned long long total, int secondsBetween, int checkInterval) {
    _total = total;
    _secondsBetween = secondsBetween;
    _checkInterval = checkInterval;

    _done = 0;
    _hits = 0;
    _lastTime = (int)time(nullptr);

    _active = true;
}


bool ProgressMeter::hit(int n) {
    if (!_active) {
        LOGV2_WARNING(23370, "hit an inactive ProgressMeter");
        return false;
    }

    _done += n;
    _hits++;
    if (_hits % _checkInterval)
        return false;

    int t = (int)time(nullptr);
    if (t - _lastTime < _secondsBetween)
        return false;

    if (_total > 0) {
        std::string stashedName = getName();
        int per = (int)(((double)_done * 100.0) / (double)_total);
        logv2::DynamicAttributes attrs;
        attrs.add("name", stashedName);
        attrs.add("done", _done);
        if (_showTotal) {
            attrs.add("total", _total);
            attrs.add("percent", per);
        }
        if (!_units.empty()) {
            attrs.add("units", _units);
        }
        LOGV2(51773, "progress meter", attrs);
    }
    _lastTime = t;
    return true;
}

std::string ProgressMeter::toString() const {
    if (!_active)
        return "";
    std::stringstream buf;
    if (_total) {
        buf << getName() << ": " << _done << '/' << _total << ' ' << (_done * 100) / _total << '%';
    } else {
        buf << getName() << ": not started";
    }

    if (!_units.empty()) {
        buf << " (" << _units << ")" << std::endl;
    }

    return buf.str();
}
}  // namespace mongo
