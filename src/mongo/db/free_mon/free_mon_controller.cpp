/**
 * Copyright (C) 2018 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl

#include "mongo/platform/basic.h"

#include "mongo/db/free_mon/free_mon_controller.h"

#include "mongo/db/ftdc/collector.h"
#include "mongo/logger/logstream_builder.h"
#include "mongo/util/log.h"

namespace mongo {

namespace {

const auto getFreeMonController =
    ServiceContext::declareDecoration<std::unique_ptr<FreeMonController>>();

}  // namespace

FreeMonController* FreeMonController::get(ServiceContext* serviceContext) {
    return getFreeMonController(serviceContext).get();
}

void FreeMonController::set(ServiceContext* serviceContext,
                            std::unique_ptr<FreeMonController> controller) {
    getFreeMonController(serviceContext) = std::move(controller);
}


FreeMonNetworkInterface::~FreeMonNetworkInterface() = default;

void FreeMonController::addRegistrationCollector(
    std::unique_ptr<FreeMonCollectorInterface> collector) {
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        invariant(_state == State::kNotStarted);

        _registrationCollectors.add(std::move(collector));
    }
}

void FreeMonController::addMetricsCollector(std::unique_ptr<FreeMonCollectorInterface> collector) {
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        invariant(_state == State::kNotStarted);

        _metricCollectors.add(std::move(collector));
    }
}

void FreeMonController::registerServerStartup(RegistrationType registrationType,
                                              std::vector<std::string>& tags) {
    _enqueue(FreeMonMessageWithPayload<FreeMonMessageType::RegisterServer>::createNow(
        FreeMonMessageWithPayload<FreeMonMessageType::RegisterServer>::payload_type(
            registrationType, tags)));
}

boost::optional<Status> FreeMonController::registerServerCommand(Milliseconds timeout) {
    auto msg = FreeMonRegisterCommandMessage::createNow(std::vector<std::string>());
    _enqueue(msg);

    if (timeout > Milliseconds::min()) {
        return msg->wait_for(timeout);
    }

    return Status::OK();
}

boost::optional<Status> FreeMonController::unregisterServerCommand(Milliseconds timeout) {
    auto msg =
        FreeMonWaitableMessageWithPayload<FreeMonMessageType::UnregisterCommand>::createNow(true);
    _enqueue(msg);

    if (timeout > Milliseconds::min()) {
        return msg->wait_for(timeout);
    }

    return Status::OK();
}

void FreeMonController::notifyOnUpsert(const BSONObj& doc) {
    invariant(doc.isOwned());
    _enqueue(FreeMonMessageWithPayload<FreeMonMessageType::NotifyOnUpsert>::createNow(doc));
}


void FreeMonController::notifyOnDelete() {
    _enqueue(FreeMonMessage::createNow(FreeMonMessageType::NotifyOnDelete));
}


void FreeMonController::notifyOnTransitionToPrimary() {
    _enqueue(FreeMonMessage::createNow(FreeMonMessageType::OnTransitionToPrimary));
}

void FreeMonController::notifyOnRollback() {
    _enqueue(FreeMonMessage::createNow(FreeMonMessageType::NotifyOnRollback));
}

void FreeMonController::_enqueue(std::shared_ptr<FreeMonMessage> msg) {
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        invariant(_state == State::kStarted);
    }

    _processor->enqueue(std::move(msg));
}

void FreeMonController::start(RegistrationType registrationType, std::vector<std::string>& tags) {
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);

        invariant(_state == State::kNotStarted);
    }

    // Start the agent
    _processor = std::make_shared<FreeMonProcessor>(
        _registrationCollectors, _metricCollectors, _network.get(), _useCrankForTest);

    _thread = stdx::thread([this] { _processor->run(); });

    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);

        invariant(_state == State::kNotStarted);
        _state = State::kStarted;
    }

    if (registrationType != RegistrationType::DoNotRegister) {
        registerServerStartup(registrationType, tags);
    }
}

void FreeMonController::stop() {
    // Stop the agent
    log() << "Shutting down free monitoring";

    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);

        bool started = (_state == State::kStarted);

        invariant(_state == State::kNotStarted || _state == State::kStarted);

        if (!started) {
            _state = State::kDone;
            return;
        }

        _state = State::kStopRequested;

        // Tell the processor to stop
        _processor->stop();
    }

    _thread.join();

    _state = State::kDone;
}

void FreeMonController::turnCrankForTest(size_t countMessagesToIgnore) {
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        invariant(_state == State::kStarted);
    }

    log() << "Turning Crank: " << countMessagesToIgnore;

    _processor->turnCrankForTest(countMessagesToIgnore);
}

void FreeMonController::getStatus(OperationContext* opCtx, BSONObjBuilder* status) {
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);

        if (_state != State::kStarted) {
            status->append("state", "disabled");
            return;
        }
    }

    _processor->getStatus(opCtx, status, FreeMonProcessor::FreeMonGetStatusEnum::kCommandStatus);
}

void FreeMonController::getServerStatus(OperationContext* opCtx, BSONObjBuilder* status) {
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);

        if (_state != State::kStarted) {
            status->append("state", "disabled");
            return;
        }
    }

    _processor->getStatus(opCtx, status, FreeMonProcessor::FreeMonGetStatusEnum::kServerStatus);
}

}  // namespace mongo
