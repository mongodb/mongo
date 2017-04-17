/**
 * Copyright (C) 2016 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kFTDC

#include "mongo/platform/basic.h"

#include "mongo/util/perfctr_collect.h"

#include "mongo/base/init.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/text.h"

namespace mongo {

namespace {

// Handle to the PDH library so that we can format error messages.
HANDLE hPdhLibrary = nullptr;

/**
 * Load PDH.DLL for good error messages.
 */
MONGO_INITIALIZER(PdhInit)(InitializerContext* context) {
    hPdhLibrary = LoadLibraryW(L"pdh.dll");
    if (nullptr == hPdhLibrary) {
        DWORD gle = GetLastError();
        return {ErrorCodes::WindowsPdhError,
                str::stream() << "LoadLibrary of pdh.dll failed with "
                              << errnoWithDescription(gle)};
    }

    return Status::OK();
}

/**
 * Output an error message for ether PDH or the system.
 */
std::string errnoWithPdhDescription(PDH_STATUS status) {
    LPWSTR errorText = nullptr;

    if (!FormatMessageW(
            FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_FROM_SYSTEM |
                FORMAT_MESSAGE_ALLOCATE_BUFFER,
            hPdhLibrary,
            status,
            0,
            reinterpret_cast<LPWSTR>(
                &errorText),  // fudge the type so FormatMessageW uses it as an out parameter.
            0,
            nullptr)) {
        DWORD gle = GetLastError();
        return str::stream() << "Format message failed with " << gle << " for status " << status;
    }

    ScopeGuard errorTextGuard = MakeGuard([errorText]() { LocalFree(errorText); });
    std::string utf8ErrorText = toUtf8String(errorText);

    auto size = utf8ErrorText.find_first_of("\r\n");
    if (size == std::string::npos) {
        size = utf8ErrorText.length();
    }

    return utf8ErrorText.substr(0, size);
}

/**
 * Format an error message for a PDH function call failure.
 */
std::string formatFunctionCallError(StringData functionName, PDH_STATUS status) {
    return str::stream() << functionName << " failed with '" << errnoWithPdhDescription(status)
                         << "'";
}

/**
 * Transform a vector of string data into a vector of strings.
 */
void transformStringDataVector(const std::vector<StringData>& input,
                               std::vector<std::string>* output) {
    output->reserve(input.size());
    for (const auto& str : input) {
        output->emplace_back(str.toString());
    }
}

/**
 * Check if a counter depends on system ticks per second to compute its value from raw values. This
 * is basically any counter that does not use 100NS as a base. FYI, if we query raw count counters,
 * we will get the system ticks as a time base.
 */
bool counterHasTickBasedTimeBase(uint32_t type) {
    return ((type & PERF_TYPE_COUNTER) == PERF_TYPE_COUNTER) &&
        ((type & PERF_TIMER_100NS) != PERF_TIMER_100NS);
}

}  // namespace

StatusWith<std::vector<std::string>> PerfCounterCollection::checkCounters(
    StringData name, const std::vector<StringData>& paths) {
    if (_counters.find(name.toString()) != _counters.end() ||
        _nestedCounters.find(name.toString()) != _nestedCounters.end()) {
        return Status(ErrorCodes::BadValue, str::stream() << "Duplicate group name for " << name);
    }

    std::vector<std::string> stringPaths;
    transformStringDataVector(paths, &stringPaths);

    // While duplicate counter paths are not a problem for PDH, they are a waste of time.
    std::sort(stringPaths.begin(), stringPaths.end());

    if (std::unique(stringPaths.begin(), stringPaths.end()) != stringPaths.end()) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Duplicate counters in paths specified");
    }

    return {stringPaths};
}

Status PerfCounterCollection::addCountersGroup(StringData name,
                                               const std::vector<StringData>& paths) {
    auto swCounters = checkCounters(name, paths);
    if (!swCounters.getStatus().isOK()) {
        return swCounters.getStatus();
    }

    _counters.emplace(name.toString(), std::move(swCounters.getValue()));

    return Status::OK();
}

Status PerfCounterCollection::addCountersGroupedByInstanceName(
    StringData name, const std::vector<StringData>& paths) {
    auto swCounters = checkCounters(name, paths);
    if (!swCounters.getStatus().isOK()) {
        return swCounters.getStatus();
    }

    _nestedCounters.emplace(name.toString(), std::move(swCounters.getValue()));

    return Status::OK();
}

StatusWith<std::unique_ptr<PerfCounterCollector>> PerfCounterCollector::create(
    PerfCounterCollection builder) {
    auto pcc = std::unique_ptr<PerfCounterCollector>(new PerfCounterCollector());

    Status s = pcc->open();
    if (!s.isOK()) {
        return s;
    }

    for (const auto& kvp : builder._counters) {
        s = pcc->addCountersGroup(kvp.first, kvp.second);
        if (!s.isOK()) {
            return s;
        }
    }

    // Sort to enforce predictable output in final document
    std::sort(pcc->_counters.begin(),
              pcc->_counters.end(),
              [](const CounterGroup& a, const CounterGroup& b) { return a.name < b.name; });

    for (const auto& kvp : builder._nestedCounters) {
        s = pcc->addCountersGroupedByInstanceName(kvp.first, kvp.second);
        if (!s.isOK()) {
            return s;
        }
    }

    std::sort(
        pcc->_nestedCounters.begin(),
        pcc->_nestedCounters.end(),
        [](const NestedCounterGroup& a, const NestedCounterGroup& b) { return a.name < b.name; });

    pcc->checkForTicksTimeBase();

    return {std::move(pcc)};
}

PerfCounterCollector::~PerfCounterCollector() {
    /*ignore*/ PdhCloseQuery(_query);
}

Status PerfCounterCollector::open() {
    PDH_STATUS status = PdhOpenQueryW(nullptr, NULL, &_query);
    if (status != ERROR_SUCCESS) {
        return {ErrorCodes::WindowsPdhError, formatFunctionCallError("PdhOpenQueryW", status)};
    }

    return Status::OK();
}

StatusWith<PerfCounterCollector::CounterInfo> PerfCounterCollector::addCounter(StringData path) {
    PDH_HCOUNTER counter{0};

    PDH_STATUS status =
        PdhAddCounterW(_query, toNativeString(path.toString().c_str()).c_str(), NULL, &counter);

    if (status != ERROR_SUCCESS) {
        return {ErrorCodes::WindowsPdhError, formatFunctionCallError("PdhAddCounterW", status)};
    }

    DWORD bufferSize = 0;

    status = PdhGetCounterInfoW(counter, false, &bufferSize, nullptr);

    if (status != PDH_MORE_DATA) {
        return {ErrorCodes::WindowsPdhError, formatFunctionCallError("PdhGetCounterInfoW", status)};
    }

    auto buf = stdx::make_unique<char[]>(bufferSize);
    auto counterInfo = reinterpret_cast<PPDH_COUNTER_INFO>(buf.get());

    status = PdhGetCounterInfoW(counter, false, &bufferSize, counterInfo);

    if (status != ERROR_SUCCESS) {
        return {ErrorCodes::WindowsPdhError, formatFunctionCallError("PdhGetCounterInfoW", status)};
    }

    // A full qualified path is as such:
    // "\\MYMACHINE\\Processor(0)\\% Idle Time"
    // MachineName \\ Object Name (Instance Name) \\ CounterName
    // Ex:
    //  MachineName: MYMACHINE
    //  Object Name: Processor
    //  InstanceName: 0
    //  CounterName: % Idle Time
    // We do not want to use Machine Name, but sometimes we want InstanceName
    //
    std::string firstName = str::stream() << '\\' << toUtf8String(counterInfo->szObjectName) << '\\'
                                          << toUtf8String(counterInfo->szCounterName);

    // Compute a second name
    std::string secondName(firstName);

    bool hasSecondValue = false;

    // Only include base for counters that need it
    if ((counterInfo->dwType & PERF_COUNTER_PRECISION) == PERF_COUNTER_PRECISION) {
        secondName += " Base";
        hasSecondValue = true;
    }

    // InstanceName is null for counters without instance names
    return {CounterInfo{std::move(firstName),
                        std::move(secondName),
                        hasSecondValue,
                        counterInfo->szInstanceName ? toUtf8String(counterInfo->szInstanceName)
                                                    : std::string(),
                        counterInfo->dwType,
                        counter}};
}

StatusWith<std::vector<PerfCounterCollector::CounterInfo>> PerfCounterCollector::addCounters(
    StringData path) {
    std::wstring pathWide = toNativeString(path.toString().c_str());
    DWORD pathListLength = 0;
    PDH_STATUS status = PdhExpandCounterPathW(pathWide.c_str(), nullptr, &pathListLength);

    if (status != PDH_MORE_DATA) {
        return {ErrorCodes::WindowsPdhError,
                str::stream() << formatFunctionCallError("PdhExpandCounterPathW", status)
                              << " for counter '" << path << "'"};
    }

    auto buf = stdx::make_unique<wchar_t[]>(pathListLength);

    status = PdhExpandCounterPathW(pathWide.c_str(), buf.get(), &pathListLength);

    if (status != ERROR_SUCCESS) {
        return {ErrorCodes::WindowsPdhError,
                formatFunctionCallError("PdhExpandCounterPathW", status)};
    }

    std::vector<CounterInfo> counters;

    // Windows' PdhExpandWildCardPathW returns a nullptr terminated array of nullptr separated
    // strings.
    std::vector<std::string> counterNames;

    const wchar_t* ptr = buf.get();
    while (ptr && *ptr) {
        counterNames.emplace_back(toUtf8String(ptr));
        ptr += wcslen(ptr) + 1;
    }

    // Sort to ensure we have a predictable ordering in the final BSON
    std::sort(counterNames.begin(), counterNames.end());

    for (const auto& name : counterNames) {
        auto swCounterInfo = addCounter(name);
        if (!swCounterInfo.isOK()) {
            return swCounterInfo.getStatus();
        }

        counters.emplace_back(std::move(swCounterInfo.getValue()));
    }

    return {std::move(counters)};
}

Status PerfCounterCollector::addCountersGroup(StringData groupName,
                                              const std::vector<std::string>& paths) {
    CounterGroup group;
    group.name = groupName.toString();

    for (const auto& path : paths) {
        auto swCounters = addCounters(path.c_str());
        if (!swCounters.isOK()) {
            return swCounters.getStatus();
        }

        auto newCounters = swCounters.getValue();

        std::copy(newCounters.begin(), newCounters.end(), std::back_inserter(group.counters));
    }

    _counters.emplace_back(group);

    return Status::OK();
}

Status PerfCounterCollector::addCountersGroupedByInstanceName(
    StringData groupName, const std::vector<std::string>& paths) {
    NestedCounterGroup group;
    group.name = groupName.toString();

    for (const auto& path : paths) {
        auto swCounters = addCounters(path.c_str());
        if (!swCounters.isOK()) {
            return swCounters.getStatus();
        }

        auto newCounters = swCounters.getValue();

        for (const auto& counter : newCounters) {
            // Verify the counter has an instance name.
            if (counter.instanceName.empty()) {
                return {ErrorCodes::BadValue,
                        str::stream() << "Counter '" << counter.firstName
                                      << "' must be an instance specific counter."};
            }

            // Skip counters in the _Total instance category.
            if (counter.instanceName == "_Total") {
                continue;
            }

            group.counters[counter.instanceName].emplace_back(std::move(counter));
        }
    }

    _nestedCounters.emplace_back(group);

    return Status::OK();
}

Status PerfCounterCollector::collectCounters(const std::vector<CounterInfo>& counters,
                                             BSONObjBuilder* builder) {
    for (const auto& counterInfo : counters) {
        DWORD dwType = 0;

        // Elapsed Time is an unusual counter in that being able to control the sample period for
        // the counter is uninteresting even though it is computed from two values. Just return the
        // computed value instead.
        if (counterInfo.type == PERF_ELAPSED_TIME) {
            PDH_FMT_COUNTERVALUE counterValue = {0};
            PDH_STATUS status = PdhGetFormattedCounterValue(
                counterInfo.handle, PDH_FMT_DOUBLE, &dwType, &counterValue);
            if (status != ERROR_SUCCESS) {
                return {ErrorCodes::WindowsPdhError,
                        formatFunctionCallError("PdhGetFormattedCounterValue", status)};
            }

            builder->append(counterInfo.firstName, counterValue.doubleValue);

        } else {
            PDH_RAW_COUNTER rawCounter = {0};
            PDH_STATUS status = PdhGetRawCounterValue(counterInfo.handle, &dwType, &rawCounter);
            if (status != ERROR_SUCCESS) {
                return {ErrorCodes::WindowsPdhError,
                        formatFunctionCallError("PdhGetRawCounterValue", status)};
            }

            if (counterInfo.hasSecondValue) {
                // Precise counters require the second value in the raw counter information
                builder->append(counterInfo.firstName, rawCounter.FirstValue);
                builder->append(counterInfo.secondName, rawCounter.SecondValue);
            } else {
                builder->append(counterInfo.firstName, rawCounter.FirstValue);
            }
        }
    }

    return Status::OK();
}

void PerfCounterCollector::checkForTicksTimeBase() {
    for (const auto& counterGroup : _counters) {
        for (const auto& counter : counterGroup.counters) {
            if (counterHasTickBasedTimeBase(counter.type)) {
                _timeBaseTicksCounter = &counter;
                return;
            }
        }
    }

    for (const auto& counterGroup : _nestedCounters) {
        for (const auto& instanceNamePair : counterGroup.counters) {
            for (const auto& counter : instanceNamePair.second) {
                if (counterHasTickBasedTimeBase(counter.type)) {
                    _timeBaseTicksCounter = &counter;
                    return;
                }
            }
        }
    }
}

Status PerfCounterCollector::collect(BSONObjBuilder* builder) {
    // Ask PDH to collect the counters
    PDH_STATUS status = PdhCollectQueryData(_query);
    if (status != ERROR_SUCCESS) {
        return {ErrorCodes::WindowsPdhError,
                formatFunctionCallError("PdhCollectQueryData", status)};
    }

    // Output timebase
    // Counters that are based on time either use 100NS or System Ticks Per Second.
    // We only need to output system ticks per second once if any counter depends on it.
    // This is typically 3320310.
    if (_timeBaseTicksCounter) {
        int64_t timebase;

        status = PdhGetCounterTimeBase(_timeBaseTicksCounter->handle, &timebase);
        if (status != ERROR_SUCCESS) {
            return {ErrorCodes::WindowsPdhError,
                    formatFunctionCallError("PdhGetCounterTimeBase", status)};
        }

        builder->append("timebase", timebase);
    }

    // Retrieve all the values that PDH collected for us.
    for (const auto& counterGroup : _counters) {
        BSONObjBuilder subObjBuilder(builder->subobjStart(counterGroup.name));

        Status s = collectCounters(counterGroup.counters, &subObjBuilder);
        if (!s.isOK()) {
            return s;
        }

        subObjBuilder.doneFast();
    }

    for (const auto& counterGroup : _nestedCounters) {
        BSONObjBuilder subObjBuilder(builder->subobjStart(counterGroup.name));

        for (const auto& instanceNamePair : counterGroup.counters) {
            BSONObjBuilder instSubObjBuilder(builder->subobjStart(instanceNamePair.first));

            Status s = collectCounters(instanceNamePair.second, &instSubObjBuilder);
            if (!s.isOK()) {
                return s;
            }

            instSubObjBuilder.doneFast();
        }

        subObjBuilder.doneFast();
    }

    return Status::OK();
}

}  // namespace mongo
