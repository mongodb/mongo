/**
 *    Copyright (C) 2015 MongoDB Inc.
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
#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/util/net/hostname_canonicalization_worker.h"

#include "mongo/db/service_context.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/log.h"
#include "mongo/util/net/hostname_canonicalization.h"
#include "mongo/util/net/sock.h"
#include "mongo/util/time_support.h"

namespace mongo {

namespace {
const auto getCanonicalizerTask =
    ServiceContext::declareDecoration<std::unique_ptr<HostnameCanonicalizationWorker>>();
}  // namespace

void HostnameCanonicalizationWorker::start(ServiceContext* service) {
    auto& task = getCanonicalizerTask(service);
    task = stdx::make_unique<HostnameCanonicalizationWorker>();
}

HostnameCanonicalizationWorker::HostnameCanonicalizationWorker() {
    _canonicalizationThread = stdx::thread(&HostnameCanonicalizationWorker::_doWork, this);
}

void HostnameCanonicalizationWorker::_doWork() {
    setThreadName("HostnameCanonicalizationWorker");
    log() << "Starting hostname canonicalization worker";
    try {
        while (true) {
            LOG(5) << "Hostname Canonicalizer is acquiring host FQDNs";
            auto fqdns =
                getHostFQDNs(getHostNameCached(), HostnameCanonicalizationMode::kForwardAndReverse);
            {
                stdx::lock_guard<stdx::mutex> lock(_canonicalizationMutex);
                _cachedFQDNs = std::move(fqdns);
            }
            LOG(5) << "Hostname Canonicalizer acquired FQDNs";

            sleepsecs(60);
        }
    } catch (...) {
        stdx::lock_guard<stdx::mutex> lock(_canonicalizationMutex);
        error() << "Unexpected fault has terminated hostname canonicalization worker: "
                << exceptionToStatus();
        error() << "serverStatus will not report FQDNs until next server restart";
        _cachedFQDNs.clear();
    }
}

std::vector<std::string> HostnameCanonicalizationWorker::getCanonicalizedFQDNs() const {
    stdx::lock_guard<stdx::mutex> lock(_canonicalizationMutex);
    return _cachedFQDNs;
}

HostnameCanonicalizationWorker* HostnameCanonicalizationWorker::get(ServiceContext* context) {
    return getCanonicalizerTask(context).get();
}

}  // namespace mongo
