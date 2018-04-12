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

#include "mongo/db/free_mon/free_mon_processor.h"

#include <functional>
#include <tuple>
#include <utility>

#include "mongo/base/data_range.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/free_mon/free_mon_storage.h"
#include "mongo/db/service_context.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {

namespace {

constexpr auto kProtocolVersion = 1;

constexpr auto kRegistrationIdMaxLength = 4096;
constexpr auto kInformationalURLMaxLength = 4096;
constexpr auto kInformationalMessageMaxLength = 4096;
constexpr auto kUserReminderMaxLength = 4096;

constexpr auto kReportingIntervalMinutesMin = 1;
constexpr auto kReportingIntervalMinutesMax = 60 * 60 * 24;

int64_t randomJitter(PseudoRandom& random, int64_t min, int64_t max) {
    dassert(max > min);
    return (std::abs(random.nextInt64()) % (max - min)) + min;
}

}  // namespace

void RegistrationRetryCounter::reset() {
    _current = _min;
    _base = _min;
    _retryCount = 0;
    _total = Hours(0);
}

bool RegistrationRetryCounter::incrementError() {
    if (_retryCount < kStage1RetryCountMax) {
        _base = 2 * _base;
        _current = _base + Seconds(randomJitter(_random, kStage1JitterMin, kStage1JitterMax));
        ++_retryCount;
    } else {
        _base = _base;
        _current = _base + Seconds(randomJitter(_random, kStage2JitterMin, kStage2JitterMax));
    }

    _total += _current;

    if (_total > kStage2DurationMax) {
        return false;
    }

    return true;
}

void FreeMonProcessor::enqueue(std::shared_ptr<FreeMonMessage> msg) {
    _queue.enqueue(std::move(msg));
}

void FreeMonProcessor::stop() {
    _queue.stop();
}

void FreeMonProcessor::run() {
    try {

        Client::initThread("free_mon");
        Client* client = &cc();

        while (true) {
            auto item = _queue.dequeue(client->getServiceContext()->getPreciseClockSource());
            if (!item.is_initialized()) {
                // Shutdown was triggered
                return;
            }

            // Do work here
            switch (item.get()->getType()) {
                case FreeMonMessageType::RegisterCommand: {
                    doCommandRegister(client, item.get());
                    break;
                }
                case FreeMonMessageType::RegisterServer: {
                    doServerRegister(
                        client,
                        checked_cast<
                            FreeMonMessageWithPayload<FreeMonMessageType::RegisterServer>*>(
                            item.get().get()));
                    break;
                }
                case FreeMonMessageType::AsyncRegisterComplete: {
                    doAsyncRegisterComplete(
                        client,
                        checked_cast<
                            FreeMonMessageWithPayload<FreeMonMessageType::AsyncRegisterComplete>*>(
                            item.get().get()));
                    break;
                }
                case FreeMonMessageType::AsyncRegisterFail: {
                    doAsyncRegisterFail(
                        client,
                        checked_cast<
                            FreeMonMessageWithPayload<FreeMonMessageType::AsyncRegisterFail>*>(
                            item.get().get()));
                    break;
                }
                default:
                    MONGO_UNREACHABLE;
            }
        }
    } catch (...) {
        // Stop the queue
        _queue.stop();

        warning() << "Uncaught exception in '" << exceptionToStatus()
                  << "' in free monitoring subsystem. Shutting down the "
                     "free monitoring subsystem.";
    }
}

void FreeMonProcessor::readState(Client* client) {

    auto optCtx = client->makeOperationContext();

    auto state = FreeMonStorage::read(optCtx.get());

    _lastReadState = state;

    if (state.is_initialized()) {
        invariant(state.get().getVersion() == kProtocolVersion);

        _state = state.get();
    } else if (!state.is_initialized()) {
        // Default the state
        _state.setVersion(kProtocolVersion);
        _state.setState(StorageStateEnum::enabled);
        _state.setRegistrationId("");
        _state.setInformationalURL("");
        _state.setMessage("");
        _state.setUserReminder("");
    }
}

void FreeMonProcessor::writeState(Client* client) {

    // Do a compare and swap
    // Verify the document is the same as the one on disk, if it is the same, then do the update
    // If the local document is different, then oh-well we do nothing, and wait until the next round

    // Has our in-memory state changed, if so consider writing
    if (_lastReadState != _state) {

        // The read and write are bound the same operation context
        {
            auto optCtx = client->makeOperationContext();

            auto state = FreeMonStorage::read(optCtx.get());

            // If our in-memory copy matches the last read, then write it to disk
            if (state == _lastReadState) {
                FreeMonStorage::replace(optCtx.get(), _state);
            }
        }
    }
}

void FreeMonProcessor::doServerRegister(
    Client* client, const FreeMonMessageWithPayload<FreeMonMessageType::RegisterServer>* msg) {

    // If we are asked to register now, then kick off a registration request
    if (msg->getPayload().first == RegistrationType::RegisterOnStart) {
        enqueue(FreeMonRegisterCommandMessage::createNow(msg->getPayload().second));
    } else if (msg->getPayload().first == RegistrationType::RegisterAfterOnTransitionToPrimary) {
        // Check if we need to wait to become primary
        // If the 'admin.system.version' has content, do not wait and just re-register
        // If the collection is empty, wait until we become primary
        //    If we become secondary, OpObserver hooks will tell us our registration id

        auto optCtx = client->makeOperationContext();

        // Check if there is an existing document
        auto state = FreeMonStorage::read(optCtx.get());

        // If there is no document, we may be in a replica set and may need to register after
        // becoming primary
        // since we cannot record the registration id until after becoming primary
        if (!state.is_initialized()) {
            // TODO: hook OnTransitionToPrimary instead of this hack
            enqueue(FreeMonRegisterCommandMessage::createNow(msg->getPayload().second));
        } else {
            // If we have state, then we can do the normal register on startup
            enqueue(FreeMonRegisterCommandMessage::createNow(msg->getPayload().second));
        }
    }
}

namespace {
template <typename T>
std::unique_ptr<Future<void>> doAsyncCallback(FreeMonProcessor* proc,
                                              Future<T> future,
                                              std::function<void(const T&)> onSuccess,
                                              std::function<void(Status)> onErrorFunc) {

    // Grab a weak_ptr to be sure that FreeMonProcessor is alive during the callback
    std::weak_ptr<FreeMonProcessor> wpProc(proc->shared_from_this());

    auto spError = std::make_shared<bool>(false);

    return std::make_unique<Future<void>>(std::move(future)
                                              .onError([=](Status s) {
                                                  *(spError.get()) = true;
                                                  if (auto spProc = wpProc.lock()) {
                                                      onErrorFunc(s);
                                                  }

                                                  return T();
                                              })
                                              .then([=](const auto& resp) {
                                                  // If we hit an error, then do not call onSuccess
                                                  if (*(spError.get()) == true) {
                                                      return;
                                                  }

                                                  // Use a shared pointer here because the callback
                                                  // could return after we disappear
                                                  if (auto spProc = wpProc.lock()) {
                                                      onSuccess(resp);
                                                  }
                                              }));
}
}  // namespace

void FreeMonProcessor::doCommandRegister(Client* client,
                                         std::shared_ptr<FreeMonMessage> sharedMsg) {
    auto msg = checked_cast<FreeMonRegisterCommandMessage*>(sharedMsg.get());

    if (_futureRegistrationResponse) {
        msg->setStatus(Status(ErrorCodes::FreeMonHttpInFlight,
                              "Free Monitoring Registration request in-flight already"));
        return;
    }

    _pendingRegisters.push_back(sharedMsg);

    readState(client);

    FreeMonRegistrationRequest req;

    if (!_state.getRegistrationId().empty()) {
        req.setId(_state.getRegistrationId());
    }

    req.setVersion(kProtocolVersion);

    if (!msg->getTags().empty()) {
        // Cache the tags for subsequent retries
        _tags = msg->getTags();
    }

    if (!_tags.empty()) {
        req.setTag(transformVector(msg->getTags()));
    }

    // Collect the data
    auto collect = _registration.collect(client);

    req.setPayload(std::get<0>(collect));

    // Send the async request
    _futureRegistrationResponse = doAsyncCallback<FreeMonRegistrationResponse>(
        this,
        _network->sendRegistrationAsync(req),
        [this](const auto& resp) {
            this->enqueue(
                FreeMonMessageWithPayload<FreeMonMessageType::AsyncRegisterComplete>::createNow(
                    resp));
        },
        [this](Status s) {
            this->enqueue(
                FreeMonMessageWithPayload<FreeMonMessageType::AsyncRegisterFail>::createNow(s));
        });
}

Status FreeMonProcessor::validateRegistrationResponse(const FreeMonRegistrationResponse& resp) {
    // Any validation failure stops registration from proceeding to upload
    if (resp.getVersion() != kProtocolVersion) {
        return Status(ErrorCodes::FreeMonHttpPermanentFailure,
                      str::stream()
                          << "Unexpected registration response protocol version, expected '"
                          << kProtocolVersion
                          << "', received '"
                          << resp.getVersion()
                          << "'");
    }

    if (resp.getId().size() >= kRegistrationIdMaxLength) {
        return Status(ErrorCodes::FreeMonHttpPermanentFailure,
                      str::stream() << "Id is '" << resp.getId().size()
                                    << "' bytes in length, maximum allowed length is '"
                                    << kRegistrationIdMaxLength
                                    << "'");
    }

    if (resp.getInformationalURL().size() >= kInformationalURLMaxLength) {
        return Status(ErrorCodes::FreeMonHttpPermanentFailure,
                      str::stream() << "InformationURL is '" << resp.getInformationalURL().size()
                                    << "' bytes in length, maximum allowed length is '"
                                    << kInformationalURLMaxLength
                                    << "'");
    }

    if (resp.getMessage().size() >= kInformationalMessageMaxLength) {
        return Status(ErrorCodes::FreeMonHttpPermanentFailure,
                      str::stream() << "Message is '" << resp.getMessage().size()
                                    << "' bytes in length, maximum allowed length is '"
                                    << kInformationalMessageMaxLength
                                    << "'");
    }

    if (resp.getUserReminder().is_initialized() &&
        resp.getUserReminder().get().size() >= kUserReminderMaxLength) {
        return Status(ErrorCodes::FreeMonHttpPermanentFailure,
                      str::stream() << "UserReminder is '" << resp.getUserReminder().get().size()
                                    << "' bytes in length, maximum allowed length is '"
                                    << kUserReminderMaxLength
                                    << "'");
    }

    if (resp.getReportingInterval() < kReportingIntervalMinutesMin ||
        resp.getReportingInterval() > kReportingIntervalMinutesMax) {
        return Status(ErrorCodes::FreeMonHttpPermanentFailure,
                      str::stream() << "Reporting Interval '" << resp.getReportingInterval()
                                    << "' must be in the range ["
                                    << kReportingIntervalMinutesMin
                                    << ","
                                    << kReportingIntervalMinutesMax
                                    << "]");
    }

    // Did cloud ask us to stop uploading?
    if (resp.getHaltMetricsUploading()) {
        return Status(ErrorCodes::FreeMonHttpPermanentFailure,
                      str::stream() << "Halting metrics upload due to response");
    }

    return Status::OK();
}


void FreeMonProcessor::notifyPendingRegisters(const Status s) {
    for (auto&& pendingRegister : _pendingRegisters) {
        (checked_cast<FreeMonRegisterCommandMessage*>(pendingRegister.get()))->setStatus(s);
    }
    _pendingRegisters.clear();
}

void FreeMonProcessor::doAsyncRegisterComplete(
    Client* client,
    const FreeMonMessageWithPayload<FreeMonMessageType::AsyncRegisterComplete>* msg) {

    // Our request is no longer in-progress so delete it
    _futureRegistrationResponse.reset();

    auto& resp = msg->getPayload();

    Status s = validateRegistrationResponse(resp);
    if (!s.isOK()) {
        warning() << "Free Monitoring registration halted due to " << s;

        notifyPendingRegisters(s);

        // If validation fails, we do not retry
        return;
    }

    // Update in-memory state
    _registrationRetry.setMin(Seconds(resp.getReportingInterval()));

    _state.setRegistrationId(resp.getId());

    if (resp.getUserReminder().is_initialized()) {
        _state.setUserReminder(resp.getUserReminder().get());
    } else {
        _state.setUserReminder("");
    }

    _state.setMessage(resp.getMessage());
    _state.setInformationalURL(resp.getInformationalURL());

    // Persist state
    writeState(client);

    // Reset retry counter
    _registrationRetry.reset();

    // Notify waiters
    notifyPendingRegisters(Status::OK());

    // TODO: Enqueue next metrics upload
    // enqueue(FreeMonMessage::createWithDeadline(FreeMonMessageType::MetricsCallTimer,
    //                                           _registrationRetry.getNextDeadline(client)));
}

void FreeMonProcessor::doAsyncRegisterFail(
    Client* client, const FreeMonMessageWithPayload<FreeMonMessageType::AsyncRegisterFail>* msg) {

    // Our request is no longer in-progress so delete it
    _futureRegistrationResponse.reset();

    if (!_registrationRetry.incrementError()) {
        // We have exceeded our retry
        warning() << "Free Monitoring is abandoning registration after excess retries";
        return;
    }

    LOG(1) << "Free Monitoring Registration Failed, " << msg->getPayload() << ", retrying in "
           << _registrationRetry.getNextDuration();

    // Enqueue a register retry
    enqueue(FreeMonRegisterCommandMessage::createWithDeadline(
        _tags, _registrationRetry.getNextDeadline(client)));
}

void FreeMonProcessor::doUnregister(Client* /*client*/) {}

}  // namespace mongo
