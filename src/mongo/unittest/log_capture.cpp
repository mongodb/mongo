/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/unittest/log_capture.h"

#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/logv2/bson_formatter.h"
#include "mongo/logv2/domain_filter.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_capture_backend.h"
#include "mongo/logv2/plain_formatter.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/synchronized_value.h"

#include <algorithm>

#include <boost/log/core/core.hpp>
#include <boost/optional.hpp>
#include <fmt/format.h>

namespace mongo::unittest {

namespace {

class Logs {
public:
    ~Logs() {
        stop();
    }

    void start() {
        invariant(!_active);
        (**_text).clear();
        (**_bson).clear();

        if (!_textSink) {
            _textSink = logv2::LogCaptureBackend::create(std::make_unique<Listener>(&_text), true);
            _textSink->set_filter(
                logv2::AllLogsFilter(logv2::LogManager::global().getGlobalDomain()));
            _textSink->set_formatter(logv2::PlainFormatter());

            _bsonSink = logv2::LogCaptureBackend::create(std::make_unique<Listener>(&_bson), false);
            _bsonSink->set_filter(
                logv2::AllLogsFilter(logv2::LogManager::global().getGlobalDomain()));
            _bsonSink->set_formatter(logv2::BSONFormatter());
        }
        _textSink->locked_backend()->setEnabled(true);
        _bsonSink->locked_backend()->setEnabled(true);
        boost::log::core::get()->add_sink(_textSink);
        boost::log::core::get()->add_sink(_bsonSink);
        _active = true;
    }

    void stop() {
        if (!_active)
            return;
        // These sinks can still emit messages after they are detached
        // from the log core. Disable them first to prevent that race.
        _textSink->locked_backend()->setEnabled(false);
        _bsonSink->locked_backend()->setEnabled(false);
        boost::log::core::get()->remove_sink(_textSink);
        boost::log::core::get()->remove_sink(_bsonSink);
        _active = false;
    }

    std::vector<std::string> getText() const {
        return **_text;
    }

    std::vector<BSONObj> getBSON() const {
        std::vector<BSONObj> objs;
        auto sync = *_bson;
        for (auto&& msg : *sync)
            objs.push_back(BSONObj(msg.c_str()));
        return objs;
    }

    size_t countTextContaining(const std::string& needle) const {
        auto sync = *_text;
        return std::count_if(sync->begin(), sync->end(), [&](auto&& s) {
            return s.find(needle) != std::string::npos;
        });
    }

    size_t countBSONContainingSubset(const BSONObj& needle) const {
        auto sync = *_bson;
        return std::count_if(sync->begin(), sync->end(), [&](auto&& msg) {
            return _isSubset(BSONObj(msg.c_str()), needle);
        });
    }

private:
    class Listener : public logv2::LogLineListener {
    public:
        explicit Listener(synchronized_value<std::vector<std::string>>* sv) : _sv(sv) {}
        void accept(const std::string& line) override {
            (***_sv).push_back(line);
        }

    private:
        synchronized_value<std::vector<std::string>>* _sv;
    };

    static bool _isSubset(BSONObj haystack, BSONObj needle) {
        for (const auto& e : needle) {
            invariant(e.type() != BSONType::array, "Cannot search for array");
            auto found = haystack[e.fieldNameStringData()];
            if (found.eoo())
                return false;
            if (e.type() == BSONType::undefined)
                continue;  // For these, we only check existence.
            if (found.canonicalType() != e.canonicalType())
                return false;
            if (e.type() == BSONType::object)
                return _isSubset(found.Obj(), e.Obj());
            if (SimpleBSONElementComparator::kInstance.compare(found, e) != 0)
                return false;
        }
        return true;
    }

    bool _active = false;
    synchronized_value<std::vector<std::string>> _text;
    synchronized_value<std::vector<std::string>> _bson;
    boost::shared_ptr<boost::log::sinks::unlocked_sink<logv2::LogCaptureBackend>> _textSink;
    boost::shared_ptr<boost::log::sinks::unlocked_sink<logv2::LogCaptureBackend>> _bsonSink;
};

Logs& globalLogs() {
    static auto&& o = *new Logs();
    return o;
}

}  // namespace

class LogCaptureGuard::Impl {
public:
    explicit Impl(bool willStart) {
        if (willStart)
            start();
    }

    ~Impl() {
        stop();
    }

    void start() {
        globalLogs().start();
    }

    void stop() {
        globalLogs().stop();
    }

    std::vector<std::string> getText() const {
        return globalLogs().getText();
    }

    std::vector<BSONObj> getBSON() const {
        return globalLogs().getBSON();
    }

    size_t countTextContaining(const std::string& needle) const {
        return globalLogs().countTextContaining(needle);
    }

    size_t countBSONContainingSubset(const BSONObj& needle) const {
        return globalLogs().countBSONContainingSubset(needle);
    }
};

LogCaptureGuard::LogCaptureGuard(bool willStart) : _impl{std::make_unique<Impl>(willStart)} {}
LogCaptureGuard::~LogCaptureGuard() = default;
void LogCaptureGuard::start() {
    _impl->start();
}
void LogCaptureGuard::stop() {
    _impl->stop();
}
std::vector<std::string> LogCaptureGuard::getText() const {
    return _impl->getText();
}
std::vector<BSONObj> LogCaptureGuard::getBSON() const {
    return _impl->getBSON();
}
size_t LogCaptureGuard::countTextContaining(const std::string& needle) const {
    return _impl->countTextContaining(needle);
}
size_t LogCaptureGuard::countBSONContainingSubset(const BSONObj& needle) const {
    return _impl->countBSONContainingSubset(needle);
}

}  // namespace mongo::unittest
