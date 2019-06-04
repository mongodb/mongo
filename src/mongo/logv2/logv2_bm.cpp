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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

#include "mongo/logv2/log.h"
#include "mongo/logger/console_appender.h"
#include "mongo/logger/logger.h"
#include "mongo/logger/message_event_utf8_encoder.h"
#include "mongo/logv2/component_settings_filter.h"
#include "mongo/logv2/log_domain_impl.h"
#include "mongo/logv2/text_formatter.h"
#include "mongo/platform/basic.h"
#include "mongo/util/log.h"

#include <benchmark/benchmark.h>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/log/sinks/text_ostream_backend.hpp>
#include <boost/make_shared.hpp>
#include <sstream>


namespace mongo {
namespace {

// Class with same interface as Console but uses a stringstream internally. So the ConsoleAppender
// can be benchmarked.
class StringstreamConsole {
public:
    stdx::mutex& mutex() {
        static stdx::mutex instance;
        return instance;
    }

    StringstreamConsole() {
        stdx::unique_lock<stdx::mutex> lk(mutex());
        lk.swap(_consoleLock);
    }

    std::ostream& out() {
        static std::stringstream _ss;
        return _ss;
    }

    void reset() {
        std::stringstream ss;
        std::swap(ss, static_cast<std::stringstream&>(out()));
    }

private:
    stdx::unique_lock<stdx::mutex> _consoleLock;
};

// RAII style helper class for init/deinit log system
class ScopedLogBench {
public:
    ScopedLogBench(bool shouldInit) : _shouldInit(shouldInit) {
        using namespace logger;
        if (_shouldInit) {
            StringstreamConsole().reset();
            globalLogManager()->detachDefaultConsoleAppender();
            _appender = globalLogDomain()->attachAppender(
                std::make_unique<ConsoleAppender<MessageEventEphemeral, StringstreamConsole>>(
                    std::make_unique<MessageEventDetailsEncoder>()));
        }
    }

    ~ScopedLogBench() {
        using namespace logger;
        if (_shouldInit) {
            globalLogDomain()->detachAppender(_appender);
            globalLogManager()->reattachDefaultConsoleAppender();
        }
    }

private:
    logger::ComponentMessageLogDomain::AppenderHandle _appender;
    bool _shouldInit;
};

// RAII style helper class for init/deinit new log system
class ScopedLogV2Bench {
public:
    ScopedLogV2Bench(bool shouldInit) : _shouldInit(shouldInit) {
        using namespace logv2;
        if (_shouldInit) {
            LogManager::global().detachDefaultBackends();

            auto backend = boost::make_shared<boost::log::sinks::text_ostream_backend>();
            backend->add_stream(boost::shared_ptr<std::ostream>(new std::stringstream()));
            backend->auto_flush(true);

            _sink = boost::make_shared<
                boost::log::sinks::synchronous_sink<boost::log::sinks::text_ostream_backend>>(
                backend);
            _sink->set_filter(
                ComponentSettingsFilter(LogManager::global().getGlobalDomain().settings()));
            _sink->set_formatter(TextFormatter());
            LogManager::global().getGlobalDomain().impl().core()->add_sink(_sink);
        }
    }

    ~ScopedLogV2Bench() {
        using namespace logv2;
        if (_shouldInit) {
            LogManager::global().getGlobalDomain().impl().core()->remove_sink(_sink);
            LogManager::global().reattachDefaultBackends();
        }
    }

private:
    boost::shared_ptr<boost::log::sinks::synchronous_sink<boost::log::sinks::text_ostream_backend>>
        _sink;
    bool _shouldInit;
};

// "Expensive" way to create a string.
std::string createLongString() {
    return std::string(1000, 'a') + std::string(1000, 'b') + std::string(1000, 'c') +
        std::string(1000, 'd') + std::string(1000, 'e');
}

static void BM_NoopLog(benchmark::State& state) {
    ScopedLogBench init(state.thread_index == 0);

    for (auto _ : state)
        MONGO_LOG(1) << "noop log";
}

static void BM_NoopLogV2Inline(benchmark::State& state) {
    ScopedLogV2Bench init(state.thread_index == 0);

    for (auto _ : state)
        LOGV2_DEBUG_INLINE(1, "noop log");
}

static void BM_NoopLogV2PimplRecord(benchmark::State& state) {
    ScopedLogV2Bench init(state.thread_index == 0);

    for (auto _ : state)
        LOGV2_DEBUG(1, "noop log");
}

static void BM_NoopLogArg(benchmark::State& state) {
    ScopedLogBench init(state.thread_index == 0);

    for (auto _ : state)
        MONGO_LOG(1) << "noop log " << createLongString();
}

static void BM_NoopLogV2InlineArg(benchmark::State& state) {
    ScopedLogV2Bench init(state.thread_index == 0);

    for (auto _ : state)
        LOGV2_DEBUG_INLINE(1, "noop log {}", "str"_attr = createLongString());
}

static void BM_NoopLogV2PimplRecordArg(benchmark::State& state) {
    ScopedLogV2Bench init(state.thread_index == 0);

    for (auto _ : state)
        LOGV2_DEBUG(1, "noop log {}", "str"_attr = createLongString());
}

static void BM_EnabledLog(benchmark::State& state) {
    ScopedLogBench init(state.thread_index == 0);

    for (auto _ : state)
        log() << "enabled log";
}


static void BM_EnabledLogV2(benchmark::State& state) {
    ScopedLogV2Bench init(state.thread_index == 0);

    for (auto _ : state)
        LOGV2("enabled log");
}

static void BM_EnabledLogExpensiveArg(benchmark::State& state) {
    ScopedLogBench init(state.thread_index == 0);

    for (auto _ : state)
        log() << "enabled log " << createLongString();
}


static void BM_EnabledLogV2ExpensiveArg(benchmark::State& state) {
    ScopedLogV2Bench init(state.thread_index == 0);

    for (auto _ : state)
        LOGV2("enabled log {}", "str"_attr = createLongString());
}

static void BM_EnabledLogManySmallArg(benchmark::State& state) {
    ScopedLogBench init(state.thread_index == 0);

    for (auto _ : state)
        log() << "enabled log " << 1 << 2 << "3" << 4.0 << "5"
              << "6"_sd << 7 << 8 << "9"
              << "10"_sd;
}


static void BM_EnabledLogV2ManySmallArg(benchmark::State& state) {
    ScopedLogV2Bench init(state.thread_index == 0);

    for (auto _ : state) {
        LOGV2("enabled log {}{}{}{}{}{}{}{}{}{}",
              "1"_attr = 1,
              "2"_attr = 2,
              "3"_attr = "3",
              "4"_attr = 4.0,
              "5"_attr = "5",
              "6"_attr = "6"_sd,
              "7"_attr = 7,
              "8"_attr = 8,
              "9"_attr = "9",
              "10"_attr = "10"_sd);
    }
}

BENCHMARK(BM_NoopLog)->Threads(1);
BENCHMARK(BM_NoopLogV2Inline)->Threads(1);
BENCHMARK(BM_NoopLogV2PimplRecord)->Threads(1);

BENCHMARK(BM_NoopLog)->Threads(2);
BENCHMARK(BM_NoopLogV2Inline)->Threads(2);
BENCHMARK(BM_NoopLogV2PimplRecord)->Threads(2);

BENCHMARK(BM_NoopLog)->Threads(4);
BENCHMARK(BM_NoopLogV2Inline)->Threads(4);
BENCHMARK(BM_NoopLogV2PimplRecord)->Threads(4);

BENCHMARK(BM_NoopLog)->Threads(8);
BENCHMARK(BM_NoopLogV2Inline)->Threads(8);
BENCHMARK(BM_NoopLogV2PimplRecord)->Threads(8);

BENCHMARK(BM_NoopLogArg)->Threads(1);
BENCHMARK(BM_NoopLogV2InlineArg)->Threads(1);
BENCHMARK(BM_NoopLogV2PimplRecordArg)->Threads(1);

BENCHMARK(BM_NoopLogArg)->Threads(2);
BENCHMARK(BM_NoopLogV2InlineArg)->Threads(2);
BENCHMARK(BM_NoopLogV2PimplRecordArg)->Threads(2);

BENCHMARK(BM_NoopLogArg)->Threads(4);
BENCHMARK(BM_NoopLogV2InlineArg)->Threads(4);
BENCHMARK(BM_NoopLogV2PimplRecordArg)->Threads(4);

BENCHMARK(BM_NoopLogArg)->Threads(8);
BENCHMARK(BM_NoopLogV2InlineArg)->Threads(8);
BENCHMARK(BM_NoopLogV2PimplRecordArg)->Threads(8);

BENCHMARK(BM_EnabledLog)->Threads(1);
BENCHMARK(BM_EnabledLogV2)->Threads(1);

BENCHMARK(BM_EnabledLog)->Threads(2);
BENCHMARK(BM_EnabledLogV2)->Threads(2);

BENCHMARK(BM_EnabledLog)->Threads(4);
BENCHMARK(BM_EnabledLogV2)->Threads(4);

BENCHMARK(BM_EnabledLog)->Threads(8);
BENCHMARK(BM_EnabledLogV2)->Threads(8);

BENCHMARK(BM_EnabledLogExpensiveArg)->Threads(1);
BENCHMARK(BM_EnabledLogV2ExpensiveArg)->Threads(1);

BENCHMARK(BM_EnabledLogExpensiveArg)->Threads(2);
BENCHMARK(BM_EnabledLogV2ExpensiveArg)->Threads(2);

BENCHMARK(BM_EnabledLogExpensiveArg)->Threads(4);
BENCHMARK(BM_EnabledLogV2ExpensiveArg)->Threads(4);

BENCHMARK(BM_EnabledLogExpensiveArg)->Threads(8);
BENCHMARK(BM_EnabledLogV2ExpensiveArg)->Threads(8);

BENCHMARK(BM_EnabledLogManySmallArg)->Threads(1);
BENCHMARK(BM_EnabledLogV2ManySmallArg)->Threads(1);

BENCHMARK(BM_EnabledLogManySmallArg)->Threads(2);
BENCHMARK(BM_EnabledLogV2ManySmallArg)->Threads(2);

BENCHMARK(BM_EnabledLogManySmallArg)->Threads(4);
BENCHMARK(BM_EnabledLogV2ManySmallArg)->Threads(4);

BENCHMARK(BM_EnabledLogManySmallArg)->Threads(8);
BENCHMARK(BM_EnabledLogV2ManySmallArg)->Threads(8);


}  // namespace
}  // namespace mongo
