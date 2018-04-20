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
#include <numeric>
#include <snappy.h>
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

constexpr Seconds kDefaultMetricsGatherInterval(1);

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
        _current = _base + Seconds(randomJitter(_random, kStage2JitterMin, kStage2JitterMax));
    }

    _total += _current;

    if (_total > kStage2DurationMax) {
        return false;
    }

    return true;
}

void MetricsRetryCounter::reset() {
    _current = _min;
    _base = _min;
    _retryCount = 0;
    _total = Hours(0);
}

bool MetricsRetryCounter::incrementError() {
    _base = static_cast<int>(pow(2, std::min(6, static_cast<int>(_retryCount)))) * _min;
    _current = _base + Seconds(randomJitter(_random, _min.count() / 2, _min.count()));
    ++_retryCount;

    _total += _current;

    if (_total > kDurationMax) {
        return false;
    }

    return true;
}

FreeMonProcessor::FreeMonProcessor(FreeMonCollectorCollection& registration,
                                   FreeMonCollectorCollection& metrics,
                                   FreeMonNetworkInterface* network,
                                   bool useCrankForTest)
    : _registration(registration),
      _metrics(metrics),
      _network(network),
      _random(Date_t::now().asInt64()),
      _registrationRetry(_random),
      _metricsRetry(_random),
      _metricsGatherInterval(kDefaultMetricsGatherInterval),
      _queue(useCrankForTest) {
    _registrationRetry.reset();
    _metricsRetry.reset();
}

void FreeMonProcessor::enqueue(std::shared_ptr<FreeMonMessage> msg) {
    _queue.enqueue(std::move(msg));
}

void FreeMonProcessor::stop() {
    _queue.stop();
}

void FreeMonProcessor::turnCrankForTest(size_t countMessagesToIgnore) {
    _countdown.reset(countMessagesToIgnore);

    _queue.turnCrankForTest(countMessagesToIgnore);

    _countdown.wait();
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

            auto msg = item.get();

            // Do work here
            switch (msg->getType()) {
                case FreeMonMessageType::RegisterCommand: {
                    doCommandRegister(client, msg);
                    break;
                }
                case FreeMonMessageType::RegisterServer: {
                    doServerRegister(
                        client,
                        checked_cast<
                            FreeMonMessageWithPayload<FreeMonMessageType::RegisterServer>*>(
                            msg.get()));
                    break;
                }
                case FreeMonMessageType::UnregisterCommand: {
                    doCommandUnregister(client,
                                        checked_cast<FreeMonWaitableMessageWithPayload<
                                            FreeMonMessageType::UnregisterCommand>*>(msg.get()));
                    break;
                }
                case FreeMonMessageType::AsyncRegisterComplete: {
                    doAsyncRegisterComplete(
                        client,
                        checked_cast<
                            FreeMonMessageWithPayload<FreeMonMessageType::AsyncRegisterComplete>*>(
                            msg.get()));
                    break;
                }
                case FreeMonMessageType::AsyncRegisterFail: {
                    doAsyncRegisterFail(
                        client,
                        checked_cast<
                            FreeMonMessageWithPayload<FreeMonMessageType::AsyncRegisterFail>*>(
                            msg.get()));
                    break;
                }
                case FreeMonMessageType::MetricsCollect: {
                    doMetricsCollect(client);
                    break;
                }
                case FreeMonMessageType::MetricsSend: {
                    doMetricsSend(client);
                    break;
                }
                case FreeMonMessageType::AsyncMetricsComplete: {
                    doAsyncMetricsComplete(
                        client,
                        checked_cast<
                            FreeMonMessageWithPayload<FreeMonMessageType::AsyncMetricsComplete>*>(
                            msg.get()));
                    break;
                }
                case FreeMonMessageType::AsyncMetricsFail: {
                    doAsyncMetricsFail(
                        client,
                        checked_cast<
                            FreeMonMessageWithPayload<FreeMonMessageType::AsyncMetricsFail>*>(
                            msg.get()));
                    break;
                }
                default:
                    MONGO_UNREACHABLE;
            }

            // Record that we have finished processing the message for testing purposes.
            _countdown.countDown();
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

                _lastReadState = _state;
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
        // Check if we need to wait to become primary:
        // If the 'admin.system.version' has content, do not wait and just re-register
        // If the collection is empty, wait until we become primary
        //    If we become secondary, OpObserver hooks will tell us our registration id

        auto optCtx = client->makeOperationContext();

        // Check if there is an existing document
        auto state = FreeMonStorage::read(optCtx.get());

        // If there is no document, we may be:
        // 1. in a replica set and may need to register after becoming primary since we cannot
        // record the registration id until after becoming primary
        // 2. a standalone which has never been registered
        //
        if (!state.is_initialized()) {
            // TODO: hook OnTransitionToPrimary
        } else {
            // We are standalone, if we have a registration id, then send a registration
            // notification, else wait for the user to register us
            if (state.get().getState() == StorageStateEnum::enabled) {
                enqueue(FreeMonRegisterCommandMessage::createNow(msg->getPayload().second));
            }
        }
    } else {
        MONGO_UNREACHABLE;
    }

    // Enqueue the first metrics gather
    enqueue(FreeMonMessage::createNow(FreeMonMessageType::MetricsCollect));
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

    if (!msg->getPayload().empty()) {
        // Cache the tags for subsequent retries
        _tags = msg->getPayload();
    }

    if (!_tags.empty()) {
        req.setTag(transformVector(msg->getPayload()));
    }

    // Collect the data
    auto collect = _registration.collect(client);

    req.setPayload(std::get<0>(collect));

    // Record that the registration is pending
    _state.setState(StorageStateEnum::pending);

    writeState(client);

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


Status FreeMonProcessor::validateMetricsResponse(const FreeMonMetricsResponse& resp) {
    // Any validation failure stops registration from proceeding to upload
    if (resp.getVersion() != kProtocolVersion) {
        return Status(ErrorCodes::FreeMonHttpPermanentFailure,
                      str::stream() << "Unexpected metrics response protocol version, expected '"
                                    << kProtocolVersion
                                    << "', received '"
                                    << resp.getVersion()
                                    << "'");
    }

    if (resp.getId().is_initialized() && resp.getId().get().size() >= kRegistrationIdMaxLength) {
        return Status(ErrorCodes::FreeMonHttpPermanentFailure,
                      str::stream() << "Id is '" << resp.getId().get().size()
                                    << "' bytes in length, maximum allowed length is '"
                                    << kRegistrationIdMaxLength
                                    << "'");
    }

    if (resp.getInformationalURL().is_initialized() &&
        resp.getInformationalURL().get().size() >= kInformationalURLMaxLength) {
        return Status(ErrorCodes::FreeMonHttpPermanentFailure,
                      str::stream() << "InformationURL is '"
                                    << resp.getInformationalURL().get().size()
                                    << "' bytes in length, maximum allowed length is '"
                                    << kInformationalURLMaxLength
                                    << "'");
    }

    if (resp.getMessage().is_initialized() &&
        resp.getMessage().get().size() >= kInformationalMessageMaxLength) {
        return Status(ErrorCodes::FreeMonHttpPermanentFailure,
                      str::stream() << "Message is '" << resp.getMessage().get().size()
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


void FreeMonProcessor::doAsyncRegisterComplete(
    Client* client,
    const FreeMonMessageWithPayload<FreeMonMessageType::AsyncRegisterComplete>* msg) {

    // Our request is no longer in-progress so delete it
    _futureRegistrationResponse.reset();

    if (_state.getState() != StorageStateEnum::pending) {
        notifyPendingRegisters(Status(ErrorCodes::BadValue, "Registration was canceled"));

        return;
    }

    auto& resp = msg->getPayload();

    Status s = validateRegistrationResponse(resp);
    if (!s.isOK()) {
        warning() << "Free Monitoring registration halted due to " << s;

        // Disable on any error
        _state.setState(StorageStateEnum::disabled);

        // Persist state
        writeState(client);

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

    _state.setState(StorageStateEnum::enabled);

    // Persist state
    writeState(client);

    // Reset retry counter
    _registrationRetry.reset();

    // Notify waiters
    notifyPendingRegisters(Status::OK());

    log() << "Free Monitoring is Enabled. Frequency: " << resp.getReportingInterval() << " seconds";

    // Enqueue next metrics upload
    enqueue(FreeMonMessage::createWithDeadline(FreeMonMessageType::MetricsSend,
                                               _registrationRetry.getNextDeadline(client)));
}

void FreeMonProcessor::doAsyncRegisterFail(
    Client* client, const FreeMonMessageWithPayload<FreeMonMessageType::AsyncRegisterFail>* msg) {

    // Our request is no longer in-progress so delete it
    _futureRegistrationResponse.reset();

    if (_state.getState() != StorageStateEnum::pending) {
        notifyPendingRegisters(Status(ErrorCodes::BadValue, "Registration was canceled"));

        return;
    }

    if (!_registrationRetry.incrementError()) {
        // We have exceeded our retry
        warning() << "Free Monitoring is abandoning registration after excess retries";
        return;
    }

    LOG(1) << "Free Monitoring Registration Failed with status '" << msg->getPayload()
           << "', retrying in " << _registrationRetry.getNextDuration();

    // Enqueue a register retry
    enqueue(FreeMonRegisterCommandMessage::createWithDeadline(
        _tags, _registrationRetry.getNextDeadline(client)));
}

void FreeMonProcessor::doCommandUnregister(
    Client* client, FreeMonWaitableMessageWithPayload<FreeMonMessageType::UnregisterCommand>* msg) {
    // Treat this request as idempotent
    if (_state.getState() != StorageStateEnum::disabled) {

        _state.setState(StorageStateEnum::disabled);

        writeState(client);

        log() << "Free Monitoring is Disabled";
    }

    msg->setStatus(Status::OK());
}

void FreeMonProcessor::doMetricsCollect(Client* client) {
    // Collect the time at the beginning so the time to collect does not affect the schedule
    Date_t now = client->getServiceContext()->getPreciseClockSource()->now();

    // Collect the data
    auto collect = _metrics.collect(client);

    _metricsBuffer.push(std::get<0>(collect));

    // Enqueue the next metrics collect based on when we started processing the last collection.
    enqueue(FreeMonMessage::createWithDeadline(FreeMonMessageType::MetricsCollect,
                                               now + _metricsGatherInterval));
}

std::string compressMetrics(MetricsBuffer& buffer) {

    std::vector<char> rawBuffer;

    size_t totalReserve = std::accumulate(
        buffer.begin(), buffer.end(), 0, [](size_t sum, auto& o) { return sum + o.objsize(); });

    rawBuffer.reserve(totalReserve);

    int count = 0;
    for (const auto& obj : buffer) {
        ++count;
        std::copy(obj.objdata(), obj.objdata() + obj.objsize(), std::back_inserter(rawBuffer));
    }

    std::string outBuffer;

    snappy::Compress(rawBuffer.data(), rawBuffer.size(), &outBuffer);

    return outBuffer;
}

void FreeMonProcessor::doMetricsSend(Client* client) {
    readState(client);

    if (_state.getState() != StorageStateEnum::enabled) {
        // If we are recently disabled, then stop sending metrics
        return;
    }

    // Build outbound request
    FreeMonMetricsRequest req;
    invariant(!_state.getRegistrationId().empty());

    req.setVersion(kProtocolVersion);
    req.setEncoding(MetricsEncodingEnum::snappy);

    req.setId(_state.getRegistrationId());

    // Get the buffered metrics
    auto metrics = compressMetrics(_metricsBuffer);
    req.setMetrics(ConstDataRange(metrics.data(), metrics.size()));

    // Send the async request
    doAsyncCallback<FreeMonMetricsResponse>(
        this,
        _network->sendMetricsAsync(req),
        [this](const auto& resp) {
            this->enqueue(
                FreeMonMessageWithPayload<FreeMonMessageType::AsyncMetricsComplete>::createNow(
                    resp));
        },
        [this](Status s) {
            this->enqueue(
                FreeMonMessageWithPayload<FreeMonMessageType::AsyncMetricsFail>::createNow(s));
        });
}

void FreeMonProcessor::doAsyncMetricsComplete(
    Client* client,
    const FreeMonMessageWithPayload<FreeMonMessageType::AsyncMetricsComplete>* msg) {

    auto& resp = msg->getPayload();

    Status s = validateMetricsResponse(resp);
    if (!s.isOK()) {
        warning() << "Free Monitoring metrics uploading halted due to " << s;

        // Disable free monitoring on validation errors
        _state.setState(StorageStateEnum::disabled);
        writeState(client);

        // If validation fails, we do not retry
        return;
    }

    // If cloud said delete, not just halt, so erase state
    if (resp.getPermanentlyDelete() == true) {
        auto opCtxUnique = client->makeOperationContext();
        FreeMonStorage::deleteState(opCtxUnique.get());

        return;
    }

    // Update in-memory state of buffered metrics
    // TODO: do we reset only the metrics we send or all pending on success?

    _metricsBuffer.reset();
    _metricsRetry.setMin(Seconds(resp.getReportingInterval()));

    if (resp.getId().is_initialized()) {
        _state.setRegistrationId(resp.getId().get());
    }

    if (resp.getUserReminder().is_initialized()) {
        _state.setUserReminder(resp.getUserReminder().get());
    }

    if (resp.getInformationalURL().is_initialized()) {
        _state.setInformationalURL(resp.getInformationalURL().get());
    }

    if (resp.getMessage().is_initialized()) {
        _state.setMessage(resp.getMessage().get());
    }

    // Persist state
    writeState(client);

    // Reset retry counter
    _metricsRetry.reset();

    // Enqueue next metrics upload
    enqueue(FreeMonMessage::createWithDeadline(FreeMonMessageType::MetricsSend,
                                               _registrationRetry.getNextDeadline(client)));
}

void FreeMonProcessor::doAsyncMetricsFail(
    Client* client, const FreeMonMessageWithPayload<FreeMonMessageType::AsyncMetricsFail>* msg) {

    if (!_metricsRetry.incrementError()) {
        // We have exceeded our retry
        warning() << "Free Monitoring is abandoning metrics upload after excess retries";
        return;
    }

    LOG(1) << "Free Monitoring Metrics upload failed with status " << msg->getPayload()
           << ", retrying in " << _metricsRetry.getNextDuration();

    // Enqueue next metrics upload
    enqueue(FreeMonMessage::createWithDeadline(FreeMonMessageType::MetricsSend,
                                               _metricsRetry.getNextDeadline(client)));
}

}  // namespace mongo
