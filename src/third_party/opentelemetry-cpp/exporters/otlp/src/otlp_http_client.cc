// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <ratio>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(HAVE_GSL)
#  include <gsl/gsl>
#else
#  include <assert.h>
#endif

#include "nlohmann/json.hpp"
#include "opentelemetry/common/timestamp.h"
#include "opentelemetry/exporters/otlp/otlp_environment.h"
#include "opentelemetry/exporters/otlp/otlp_http.h"
#include "opentelemetry/exporters/otlp/otlp_http_client.h"
#include "opentelemetry/ext/http/client/http_client.h"
#include "opentelemetry/ext/http/client/http_client_factory.h"
#include "opentelemetry/ext/http/common/url_parser.h"
#include "opentelemetry/nostd/string_view.h"
#include "opentelemetry/nostd/variant.h"
#include "opentelemetry/sdk/common/base64.h"
#include "opentelemetry/sdk/common/exporter_utils.h"
#include "opentelemetry/sdk/common/global_log_handler.h"
#include "opentelemetry/version.h"

// clang-format off
#include "opentelemetry/exporters/otlp/protobuf_include_prefix.h" // IWYU pragma: keep
// clang-format on
#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <google/protobuf/stubs/port.h>
// clang-format off
#include "opentelemetry/exporters/otlp/protobuf_include_suffix.h" // IWYU pragma: keep
// clang-format on

#ifdef GetMessage
#  undef GetMessage
#endif

namespace http_client = opentelemetry::ext::http::client;

OPENTELEMETRY_BEGIN_NAMESPACE
namespace exporter
{
namespace otlp
{

namespace
{

/**
 * This class handles the response message from the HTTP request
 */
class ResponseHandler : public http_client::EventHandler
{
public:
  /**
   * Creates a response handler, that by default doesn't display to console
   */
  ResponseHandler(std::function<bool(opentelemetry::sdk::common::ExportResult)> &&callback,
                  bool console_debug = false)
      : result_callback_{std::move(callback)}, console_debug_{console_debug}
  {}

  std::string BuildResponseLogMessage(http_client::Response &response,
                                      const std::string &body) noexcept
  {
    std::stringstream ss;
    ss << "Status:" << response.GetStatusCode() << ", Header:";
    response.ForEachHeader([&ss](opentelemetry::nostd::string_view header_name,
                                 opentelemetry::nostd::string_view header_value) {
      ss << "\t" << header_name.data() << ": " << header_value.data() << ",";
      return true;
    });
    ss << "Body:" << body;

    return ss.str();
  }

  /**
   * Automatically called when the response is received, store the body into a string and notify any
   * threads blocked on this result
   */
  void OnResponse(http_client::Response &response) noexcept override
  {
    sdk::common::ExportResult result = sdk::common::ExportResult::kSuccess;
    std::string log_message;
    // Lock the private members so they can't be read while being modified
    {
      std::unique_lock<std::mutex> lk(mutex_);

      // Store the body of the request
      body_ = std::string(response.GetBody().begin(), response.GetBody().end());

      if (!(response.GetStatusCode() >= 200 && response.GetStatusCode() <= 299))
      {
        log_message = BuildResponseLogMessage(response, body_);

        OTEL_INTERNAL_LOG_ERROR("[OTLP HTTP Client] Export failed, " << log_message);
        result = sdk::common::ExportResult::kFailure;
      }
      else if (console_debug_)
      {
        if (log_message.empty())
        {
          log_message = BuildResponseLogMessage(response, body_);
        }
        OTEL_INTERNAL_LOG_DEBUG("[OTLP HTTP Client] Export success, " << log_message);
      }
    }

    {
      bool expected = false;
      if (stopping_.compare_exchange_strong(expected, true, std::memory_order_release))
      {
        Unbind(result);
      }
    }
  }

  /**
   * Returns the body of the response
   */
  std::string GetResponseBody()
  {
    // Lock so that body_ can't be written to while returning it
    std::unique_lock<std::mutex> lk(mutex_);
    return body_;
  }

  // Callback method when an http event occurs
  void OnEvent(http_client::SessionState state,
               opentelemetry::nostd::string_view reason) noexcept override
  {
    // need to modify stopping_ under lock before calling callback
    bool need_stop = false;
    switch (state)
    {
      case http_client::SessionState::CreateFailed:
      case http_client::SessionState::ConnectFailed:
      case http_client::SessionState::SendFailed:
      case http_client::SessionState::SSLHandshakeFailed:
      case http_client::SessionState::TimedOut:
      case http_client::SessionState::NetworkError:
      case http_client::SessionState::Cancelled: {
        need_stop = true;
      }
      break;

      default:
        break;
    }

    // If any failure event occurs, release the condition variable to unblock main thread
    switch (state)
    {
      case http_client::SessionState::CreateFailed: {
        std::stringstream error_message;
        error_message << "[OTLP HTTP Client] Session state: session create failed.";
        if (!reason.empty())
        {
          error_message.write(reason.data(), reason.size());
        }
        OTEL_INTERNAL_LOG_ERROR(error_message.str());
      }
      break;

      case http_client::SessionState::Created:
        if (console_debug_)
        {
          OTEL_INTERNAL_LOG_DEBUG("[OTLP HTTP Client] Session state: session created");
        }
        break;

      case http_client::SessionState::Destroyed:
        if (console_debug_)
        {
          OTEL_INTERNAL_LOG_DEBUG("[OTLP HTTP Client] Session state: session destroyed");
        }
        break;

      case http_client::SessionState::Connecting:
        if (console_debug_)
        {
          OTEL_INTERNAL_LOG_DEBUG("[OTLP HTTP Client] Session state: connecting to peer");
        }
        break;

      case http_client::SessionState::ConnectFailed: {
        std::stringstream error_message;
        error_message << "[OTLP HTTP Client] Session state: connection failed.";
        if (!reason.empty())
        {
          error_message.write(reason.data(), reason.size());
        }
        OTEL_INTERNAL_LOG_ERROR(error_message.str());
      }
      break;

      case http_client::SessionState::Connected:
        if (console_debug_)
        {
          OTEL_INTERNAL_LOG_DEBUG("[OTLP HTTP Client] Session state: connected");
        }
        break;

      case http_client::SessionState::Sending:
        if (console_debug_)
        {
          OTEL_INTERNAL_LOG_DEBUG("[OTLP HTTP Client] Session state: sending request");
        }
        break;

      case http_client::SessionState::SendFailed: {
        std::stringstream error_message;
        error_message << "[OTLP HTTP Client] Session state: request send failed.";
        if (!reason.empty())
        {
          error_message.write(reason.data(), reason.size());
        }
        OTEL_INTERNAL_LOG_ERROR(error_message.str());
      }
      break;

      case http_client::SessionState::Response:
        if (console_debug_)
        {
          OTEL_INTERNAL_LOG_DEBUG("[OTLP HTTP Client] Session state: response received");
        }
        break;

      case http_client::SessionState::SSLHandshakeFailed: {
        std::stringstream error_message;
        error_message << "[OTLP HTTP Client] Session state: SSL handshake failed.";
        if (!reason.empty())
        {
          error_message.write(reason.data(), reason.size());
        }
        OTEL_INTERNAL_LOG_ERROR(error_message.str());
      }
      break;

      case http_client::SessionState::TimedOut: {
        std::stringstream error_message;
        error_message << "[OTLP HTTP Client] Session state: request time out.";
        if (!reason.empty())
        {
          error_message.write(reason.data(), reason.size());
        }
        OTEL_INTERNAL_LOG_ERROR(error_message.str());
      }
      break;

      case http_client::SessionState::NetworkError: {
        std::stringstream error_message;
        error_message << "[OTLP HTTP Client] Session state: network error.";
        if (!reason.empty())
        {
          error_message.write(reason.data(), reason.size());
        }
        OTEL_INTERNAL_LOG_ERROR(error_message.str());
      }
      break;

      case http_client::SessionState::ReadError:
        if (console_debug_)
        {
          OTEL_INTERNAL_LOG_DEBUG("[OTLP HTTP Client] Session state: error reading response");
        }
        break;

      case http_client::SessionState::WriteError:
        if (console_debug_)
        {
          OTEL_INTERNAL_LOG_DEBUG("[OTLP HTTP Client] Session state: error writing request");
        }
        break;

      case http_client::SessionState::Cancelled: {
        std::stringstream error_message;
        error_message << "[OTLP HTTP Client] Session state: (manually) cancelled.";
        if (!reason.empty())
        {
          error_message.write(reason.data(), reason.size());
        }
        OTEL_INTERNAL_LOG_ERROR(error_message.str());
      }
      break;

      default:
        break;
    }

    if (need_stop)
    {
      bool expected = false;
      if (stopping_.compare_exchange_strong(expected, true, std::memory_order_release))
      {
        Unbind(sdk::common::ExportResult::kFailure);
      }
    }
  }

  void Unbind(sdk::common::ExportResult result)
  {
    // ReleaseSession may destroy this object, so we need to move owner and session into stack
    // first.
    OtlpHttpClient *owner                                    = owner_;
    const opentelemetry::ext::http::client::Session *session = session_;

    owner_   = nullptr;
    session_ = nullptr;

    if (nullptr != owner && nullptr != session)
    {
      // Release the session at last
      owner->ReleaseSession(*session);

      if (result_callback_)
      {
        result_callback_(result);
      }
    }
  }

  void Bind(OtlpHttpClient *owner,
            const opentelemetry::ext::http::client::Session &session) noexcept
  {
    session_ = &session;
    owner_   = owner;
  }

private:
  // Define a mutex to keep thread safety
  std::mutex mutex_;

  // Track the owner and the binded session
  OtlpHttpClient *owner_                                    = nullptr;
  const opentelemetry::ext::http::client::Session *session_ = nullptr;

  // Whether notify has been called
  std::atomic<bool> stopping_{false};

  // A string to store the response body
  std::string body_ = "";

  // Result callback when in async mode
  std::function<bool(opentelemetry::sdk::common::ExportResult)> result_callback_;

  // Whether to print the results from the callback
  bool console_debug_ = false;
};

static inline char HexEncode(unsigned char byte)
{
#if defined(HAVE_GSL)
  Expects(byte <= 16);
#else
  assert(byte <= 16);
#endif
  if (byte >= 10)
  {
    return byte - 10 + 'a';
  }
  else
  {
    return byte + '0';
  }
}

static std::string HexEncode(const std::string &bytes)
{
  std::string ret;
  ret.reserve(bytes.size() * 2);
  for (std::string::size_type i = 0; i < bytes.size(); ++i)
  {
    unsigned char byte = static_cast<unsigned char>(bytes[i]);
    ret.push_back(HexEncode(byte >> 4));
    ret.push_back(HexEncode(byte & 0x0f));
  }
  return ret;
}

static std::string BytesMapping(const std::string &bytes,
                                const google::protobuf::FieldDescriptor *field_descriptor,
                                JsonBytesMappingKind kind)
{
  switch (kind)
  {
    case JsonBytesMappingKind::kHexId: {
      if (field_descriptor->lowercase_name() == "trace_id" ||
          field_descriptor->lowercase_name() == "span_id" ||
          field_descriptor->lowercase_name() == "parent_span_id")
      {
        return HexEncode(bytes);
      }
      else
      {
        return opentelemetry::sdk::common::Base64Escape(bytes);
      }
    }
    case JsonBytesMappingKind::kBase64: {
      // Base64 is the default bytes mapping of protobuf
      return opentelemetry::sdk::common::Base64Escape(bytes);
    }
    case JsonBytesMappingKind::kHex:
      return HexEncode(bytes);
    default:
      return bytes;
  }
}

static void ConvertGenericFieldToJson(nlohmann::json &value,
                                      const google::protobuf::Message &message,
                                      const google::protobuf::FieldDescriptor *field_descriptor,
                                      const OtlpHttpClientOptions &options);

static void ConvertListFieldToJson(nlohmann::json &value,
                                   const google::protobuf::Message &message,
                                   const google::protobuf::FieldDescriptor *field_descriptor,
                                   const OtlpHttpClientOptions &options);

// NOLINTBEGIN(misc-no-recursion)
static void ConvertGenericMessageToJson(nlohmann::json &value,
                                        const google::protobuf::Message &message,
                                        const OtlpHttpClientOptions &options)
{
  std::vector<const google::protobuf::FieldDescriptor *> fields_with_data;
  message.GetReflection()->ListFields(message, &fields_with_data);
  for (std::size_t i = 0; i < fields_with_data.size(); ++i)
  {
    const google::protobuf::FieldDescriptor *field_descriptor = fields_with_data[i];
    nlohmann::json &child_value = options.use_json_name ? value[field_descriptor->json_name()]
                                                        : value[field_descriptor->camelcase_name()];
    if (field_descriptor->is_repeated())
    {
      ConvertListFieldToJson(child_value, message, field_descriptor, options);
    }
    else
    {
      ConvertGenericFieldToJson(child_value, message, field_descriptor, options);
    }
  }
}

bool SerializeToHttpBody(http_client::Body &output, const google::protobuf::Message &message)
{
  auto body_size = message.ByteSizeLong();
  if (body_size > 0)
  {
    output.resize(body_size);
    return message.SerializeWithCachedSizesToArray(
        reinterpret_cast<google::protobuf::uint8 *>(&output[0]));
  }
  return true;
}

void ConvertGenericFieldToJson(nlohmann::json &value,
                               const google::protobuf::Message &message,
                               const google::protobuf::FieldDescriptor *field_descriptor,
                               const OtlpHttpClientOptions &options)
{
  switch (field_descriptor->cpp_type())
  {
    case google::protobuf::FieldDescriptor::CPPTYPE_INT32: {
      value = message.GetReflection()->GetInt32(message, field_descriptor);
      break;
    }
    case google::protobuf::FieldDescriptor::CPPTYPE_INT64: {
      // According to Protobuf specs 64-bit integer numbers in JSON-encoded payloads are encoded as
      // decimal strings, and either numbers or strings are accepted when decoding.
      value = std::to_string(message.GetReflection()->GetInt64(message, field_descriptor));
      break;
    }
    case google::protobuf::FieldDescriptor::CPPTYPE_UINT32: {
      value = message.GetReflection()->GetUInt32(message, field_descriptor);
      break;
    }
    case google::protobuf::FieldDescriptor::CPPTYPE_UINT64: {
      // According to Protobuf specs 64-bit integer numbers in JSON-encoded payloads are encoded as
      // decimal strings, and either numbers or strings are accepted when decoding.
      value = std::to_string(message.GetReflection()->GetUInt64(message, field_descriptor));
      break;
    }
    case google::protobuf::FieldDescriptor::CPPTYPE_STRING: {
      std::string empty;
      if (field_descriptor->type() == google::protobuf::FieldDescriptor::TYPE_BYTES)
      {
        value = BytesMapping(
            message.GetReflection()->GetStringReference(message, field_descriptor, &empty),
            field_descriptor, options.json_bytes_mapping);
      }
      else
      {
        value = message.GetReflection()->GetStringReference(message, field_descriptor, &empty);
      }
      break;
    }
    case google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE: {
      ConvertGenericMessageToJson(
          value, message.GetReflection()->GetMessage(message, field_descriptor, nullptr), options);
      break;
    }
    case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE: {
      value = message.GetReflection()->GetDouble(message, field_descriptor);
      break;
    }
    case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT: {
      value = message.GetReflection()->GetFloat(message, field_descriptor);
      break;
    }
    case google::protobuf::FieldDescriptor::CPPTYPE_BOOL: {
      value = message.GetReflection()->GetBool(message, field_descriptor);
      break;
    }
    case google::protobuf::FieldDescriptor::CPPTYPE_ENUM: {
      value = message.GetReflection()->GetEnumValue(message, field_descriptor);
      break;
    }
    default: {
      break;
    }
  }
}

void ConvertListFieldToJson(nlohmann::json &value,
                            const google::protobuf::Message &message,
                            const google::protobuf::FieldDescriptor *field_descriptor,
                            const OtlpHttpClientOptions &options)
{
  auto field_size = message.GetReflection()->FieldSize(message, field_descriptor);

  switch (field_descriptor->cpp_type())
  {
    case google::protobuf::FieldDescriptor::CPPTYPE_INT32: {
      for (int i = 0; i < field_size; ++i)
      {
        value.push_back(message.GetReflection()->GetRepeatedInt32(message, field_descriptor, i));
      }

      break;
    }
    case google::protobuf::FieldDescriptor::CPPTYPE_INT64: {
      for (int i = 0; i < field_size; ++i)
      {
        // According to Protobuf specs 64-bit integer numbers in JSON-encoded payloads are encoded
        // as decimal strings, and either numbers or strings are accepted when decoding.
        value.push_back(std::to_string(
            message.GetReflection()->GetRepeatedInt64(message, field_descriptor, i)));
      }

      break;
    }
    case google::protobuf::FieldDescriptor::CPPTYPE_UINT32: {
      for (int i = 0; i < field_size; ++i)
      {
        value.push_back(message.GetReflection()->GetRepeatedUInt32(message, field_descriptor, i));
      }

      break;
    }
    case google::protobuf::FieldDescriptor::CPPTYPE_UINT64: {
      for (int i = 0; i < field_size; ++i)
      {
        // According to Protobuf specs 64-bit integer numbers in JSON-encoded payloads are encoded
        // as decimal strings, and either numbers or strings are accepted when decoding.
        value.push_back(std::to_string(
            message.GetReflection()->GetRepeatedUInt64(message, field_descriptor, i)));
      }

      break;
    }
    case google::protobuf::FieldDescriptor::CPPTYPE_STRING: {
      std::string empty;
      if (field_descriptor->type() == google::protobuf::FieldDescriptor::TYPE_BYTES)
      {
        for (int i = 0; i < field_size; ++i)
        {
          value.push_back(BytesMapping(message.GetReflection()->GetRepeatedStringReference(
                                           message, field_descriptor, i, &empty),
                                       field_descriptor, options.json_bytes_mapping));
        }
      }
      else
      {
        for (int i = 0; i < field_size; ++i)
        {
          value.push_back(message.GetReflection()->GetRepeatedStringReference(
              message, field_descriptor, i, &empty));
        }
      }
      break;
    }
    case google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE: {
      for (int i = 0; i < field_size; ++i)
      {
        nlohmann::json sub_value;
        ConvertGenericMessageToJson(
            sub_value, message.GetReflection()->GetRepeatedMessage(message, field_descriptor, i),
            options);
        value.push_back(std::move(sub_value));
      }

      break;
    }
    case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE: {
      for (int i = 0; i < field_size; ++i)
      {
        value.push_back(message.GetReflection()->GetRepeatedDouble(message, field_descriptor, i));
      }

      break;
    }
    case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT: {
      for (int i = 0; i < field_size; ++i)
      {
        value.push_back(message.GetReflection()->GetRepeatedFloat(message, field_descriptor, i));
      }

      break;
    }
    case google::protobuf::FieldDescriptor::CPPTYPE_BOOL: {
      for (int i = 0; i < field_size; ++i)
      {
        value.push_back(message.GetReflection()->GetRepeatedBool(message, field_descriptor, i));
      }

      break;
    }
    case google::protobuf::FieldDescriptor::CPPTYPE_ENUM: {
      for (int i = 0; i < field_size; ++i)
      {
        value.push_back(
            message.GetReflection()->GetRepeatedEnumValue(message, field_descriptor, i));
      }
      break;
    }
    default: {
      break;
    }
  }
}

// NOLINTEND(misc-no-recursion) suppressing for performance, if implemented iterative process needs
// Dynamic memory allocation

}  // namespace

OtlpHttpClient::OtlpHttpClient(OtlpHttpClientOptions &&options)
    : is_shutdown_(false),
      options_(options),
      http_client_(http_client::HttpClientFactory::Create()),
      start_session_counter_(0),
      finished_session_counter_(0)
{
  http_client_->SetMaxSessionsPerConnection(options_.max_requests_per_connection);
}

OtlpHttpClient::~OtlpHttpClient()
{
  if (!IsShutdown())
  {
    Shutdown();
  }

  // Wait for all the sessions to finish
  std::unique_lock<std::mutex> lock(session_waker_lock_);
  while (true)
  {
    {
      std::lock_guard<std::recursive_mutex> guard{session_manager_lock_};
      if (running_sessions_.empty())
      {
        break;
      }
    }
    // When changes of running_sessions_ and notify_one/notify_all happen between predicate
    // checking and waiting, we should not wait forever. We should cleanup gc sessions here as soon
    // as possible to call FinishSession() and cleanup resources.
    if (std::cv_status::timeout == session_waker_.wait_for(lock, options_.timeout))
    {
      cleanupGCSessions();
    }
  }

  // And then remove all session datas
  while (cleanupGCSessions())
    ;
}

OtlpHttpClient::OtlpHttpClient(OtlpHttpClientOptions &&options,
                               std::shared_ptr<ext::http::client::HttpClient> http_client)
    : is_shutdown_(false),
      options_(std::move(options)),
      http_client_(std::move(http_client)),
      start_session_counter_(0),
      finished_session_counter_(0)
{
  http_client_->SetMaxSessionsPerConnection(options_.max_requests_per_connection);
}

// ----------------------------- HTTP Client methods ------------------------------
opentelemetry::sdk::common::ExportResult OtlpHttpClient::Export(
    const google::protobuf::Message &message) noexcept
{
  std::shared_ptr<opentelemetry::sdk::common::ExportResult> session_result =
      std::make_shared<opentelemetry::sdk::common::ExportResult>(
          opentelemetry::sdk::common::ExportResult::kSuccess);
  opentelemetry::sdk::common::ExportResult export_result = Export(
      message,
      [session_result](opentelemetry::sdk::common::ExportResult result) {
        *session_result = result;
        return result == opentelemetry::sdk::common::ExportResult::kSuccess;
      },
      0);

  if (opentelemetry::sdk::common::ExportResult::kSuccess != export_result)
  {
    return export_result;
  }

  return *session_result;
}

sdk::common::ExportResult OtlpHttpClient::Export(
    const google::protobuf::Message &message,
    std::function<bool(opentelemetry::sdk::common::ExportResult)> &&result_callback) noexcept
{
  return Export(message, std::move(result_callback), options_.max_concurrent_requests);
}

sdk::common::ExportResult OtlpHttpClient::Export(
    const google::protobuf::Message &message,
    std::function<bool(opentelemetry::sdk::common::ExportResult)> &&result_callback,
    std::size_t max_running_requests) noexcept
{
  auto session = createSession(message, std::move(result_callback));
  if (opentelemetry::nostd::holds_alternative<sdk::common::ExportResult>(session))
  {
    sdk::common::ExportResult result =
        opentelemetry::nostd::get<sdk::common::ExportResult>(session);
    if (result_callback)
    {
      result_callback(result);
    }
    return result;
  }

  addSession(std::move(opentelemetry::nostd::get<HttpSessionData>(session)));

  // Wait for the response to be received
  if (options_.console_debug)
  {
    OTEL_INTERNAL_LOG_DEBUG(
        "[OTLP HTTP Client] Waiting for response from "
        << options_.url << " (timeout = "
        << std::chrono::duration_cast<std::chrono::milliseconds>(options_.timeout).count()
        << " milliseconds)");
  }

  // Wait for any session to finish if there are to many sessions
  std::unique_lock<std::mutex> lock(session_waker_lock_);
  bool wait_successful =
      session_waker_.wait_for(lock, options_.timeout, [this, max_running_requests] {
        std::lock_guard<std::recursive_mutex> guard{session_manager_lock_};
        return running_sessions_.size() <= max_running_requests;
      });

  cleanupGCSessions();

  if (!wait_successful)
  {
    return opentelemetry::sdk::common::ExportResult::kFailure;
  }

  return opentelemetry::sdk::common::ExportResult::kSuccess;
}

bool OtlpHttpClient::ForceFlush(std::chrono::microseconds timeout) noexcept
{
  // ASAN will report chrono: runtime error: signed integer overflow: A + B cannot be represented
  //   in type 'long int' here. So we reset timeout to meet signed long int limit here.
  timeout = opentelemetry::common::DurationUtil::AdjustWaitForTimeout(
      timeout, std::chrono::microseconds::zero());

  // Wait for all the sessions to finish
  std::unique_lock<std::mutex> lock(session_waker_lock_);

  std::chrono::steady_clock::duration timeout_steady =
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(timeout);
  if (timeout_steady <= std::chrono::steady_clock::duration::zero())
  {
    timeout_steady = (std::chrono::steady_clock::duration::max)();
  }

  size_t wait_counter = start_session_counter_.load(std::memory_order_acquire);

  while (timeout_steady > std::chrono::steady_clock::duration::zero())
  {
    {
      std::lock_guard<std::recursive_mutex> guard{session_manager_lock_};
      if (running_sessions_.empty())
      {
        break;
      }
    }
    // When changes of running_sessions_ and notify_one/notify_all happen between predicate
    // checking and waiting, we should not wait forever.We should cleanup gc sessions here as soon
    // as possible to call FinishSession() and cleanup resources.
    std::chrono::steady_clock::time_point start_timepoint = std::chrono::steady_clock::now();
    if (std::cv_status::timeout == session_waker_.wait_for(lock, options_.timeout))
    {
      cleanupGCSessions();
    }
    else if (finished_session_counter_.load(std::memory_order_acquire) >= wait_counter)
    {
      break;
    }

    timeout_steady -= std::chrono::steady_clock::now() - start_timepoint;
  }

  return timeout_steady > std::chrono::steady_clock::duration::zero();
}

bool OtlpHttpClient::Shutdown(std::chrono::microseconds timeout) noexcept
{
  is_shutdown_.store(true, std::memory_order_release);

  bool force_flush_result = ForceFlush(timeout);

  {
    std::lock_guard<std::recursive_mutex> guard{session_manager_lock_};

    // Shutdown the session manager
    http_client_->CancelAllSessions();
    http_client_->FinishAllSessions();
  }

  // Wait util all sessions are canceled.
  while (cleanupGCSessions())
  {
    ForceFlush(std::chrono::milliseconds{1});
  }
  return force_flush_result;
}

void OtlpHttpClient::ReleaseSession(
    const opentelemetry::ext::http::client::Session &session) noexcept
{
  bool has_session = false;

  std::lock_guard<std::recursive_mutex> guard{session_manager_lock_};

  auto session_iter = running_sessions_.find(&session);
  if (session_iter != running_sessions_.end())
  {
    // Move session and handle into gc list, and they will be destroyed later
    gc_sessions_.emplace_back(std::move(session_iter->second));
    running_sessions_.erase(session_iter);

    finished_session_counter_.fetch_add(1, std::memory_order_release);
    has_session = true;
  }

  // Call session_waker_.notify_all() with session_manager_lock_ locked to keep session_waker_
  // available when destroying OtlpHttpClient
  if (has_session)
  {
    session_waker_.notify_all();
  }
}

opentelemetry::nostd::variant<opentelemetry::sdk::common::ExportResult,
                              OtlpHttpClient::HttpSessionData>
OtlpHttpClient::createSession(
    const google::protobuf::Message &message,
    std::function<bool(opentelemetry::sdk::common::ExportResult)> &&result_callback) noexcept
{
  // Parse uri and store it to cache
  if (http_uri_.empty())
  {
    auto parse_url = opentelemetry::ext::http::common::UrlParser(std::string(options_.url));
    if (!parse_url.success_)
    {
      std::string error_message = "[OTLP HTTP Client] Export failed, invalid url: " + options_.url;
      if (options_.console_debug)
      {
        std::cerr << error_message << '\n';
      }
      OTEL_INTERNAL_LOG_ERROR(error_message.c_str());

      return opentelemetry::sdk::common::ExportResult::kFailure;
    }

    if (!parse_url.path_.empty() && parse_url.path_[0] == '/')
    {
      http_uri_ = parse_url.path_.substr(1);
    }
    else
    {
      http_uri_ = parse_url.path_;
    }
  }

  http_client::Body body_vec;
  std::string content_type;
  if (options_.content_type == HttpRequestContentType::kBinary)
  {
    if (SerializeToHttpBody(body_vec, message))
    {
      if (options_.console_debug)
      {
        OTEL_INTERNAL_LOG_DEBUG(
            "[OTLP HTTP Client] Request body(Binary): " << message.Utf8DebugString());
      }
    }
    else
    {
      if (options_.console_debug)
      {
        OTEL_INTERNAL_LOG_DEBUG("[OTLP HTTP Client] Serialize body failed(Binary):"
                                << message.InitializationErrorString());
      }
      return opentelemetry::sdk::common::ExportResult::kFailure;
    }
    content_type = kHttpBinaryContentType;
  }
  else
  {
    nlohmann::json json_request;

    // Convert from proto into json object
    ConvertGenericMessageToJson(json_request, message, options_);

    std::string post_body_json =
        json_request.dump(-1, ' ', false, nlohmann::detail::error_handler_t::replace);
    if (options_.console_debug)
    {
      OTEL_INTERNAL_LOG_DEBUG("[OTLP HTTP Client] Request body(Json)" << post_body_json);
    }
    body_vec.assign(post_body_json.begin(), post_body_json.end());
    content_type = kHttpJsonContentType;
  }

  // Send the request
  std::lock_guard<std::recursive_mutex> guard{session_manager_lock_};
  // Return failure if this exporter has been shutdown
  if (IsShutdown())
  {
    const char *error_message = "[OTLP HTTP Client] Export failed, exporter is shutdown";
    if (options_.console_debug)
    {
      std::cerr << error_message << '\n';
    }
    OTEL_INTERNAL_LOG_ERROR(error_message);

    return opentelemetry::sdk::common::ExportResult::kFailure;
  }

  auto session = http_client_->CreateSession(options_.url);
  auto request = session->CreateRequest();

  for (auto &header : options_.http_headers)
  {
    request->AddHeader(header.first,
                       opentelemetry::ext::http::common::UrlDecoder::Decode(header.second));
  }
  request->SetUri(http_uri_);
  request->SetSslOptions(options_.ssl_options);
  request->SetTimeoutMs(std::chrono::duration_cast<std::chrono::milliseconds>(options_.timeout));
  request->SetMethod(http_client::Method::Post);
  request->SetBody(body_vec);
  request->ReplaceHeader("Content-Type", content_type);
  request->ReplaceHeader("User-Agent", options_.user_agent);

  if (options_.compression == "gzip")
  {
    request->SetCompression(opentelemetry::ext::http::client::Compression::kGzip);
  }

  // Returns the created session data
  return HttpSessionData{
      std::move(session),
      std::shared_ptr<opentelemetry::ext::http::client::EventHandler>{
          new ResponseHandler(std::move(result_callback), options_.console_debug)}};
}

void OtlpHttpClient::addSession(HttpSessionData &&session_data) noexcept
{
  if (!session_data.session || !session_data.event_handle)
  {
    return;
  }

  std::shared_ptr<opentelemetry::ext::http::client::Session> session = session_data.session;
  std::shared_ptr<opentelemetry::ext::http::client::EventHandler> handle =
      session_data.event_handle;
  {
    std::lock_guard<std::recursive_mutex> guard{session_manager_lock_};
    static_cast<ResponseHandler *>(handle.get())->Bind(this, *session);

    HttpSessionData &store_session_data = running_sessions_[session.get()];
    store_session_data                  = std::move(session_data);
  }

  start_session_counter_.fetch_add(1, std::memory_order_release);
  // Send request after the session is added
  session->SendRequest(handle);
}

bool OtlpHttpClient::cleanupGCSessions() noexcept
{
  std::lock_guard<std::recursive_mutex> guard{session_manager_lock_};
  std::list<HttpSessionData> gc_sessions;
  gc_sessions_.swap(gc_sessions);

  for (auto &session_data : gc_sessions)
  {
    // FinishSession must be called with same thread and before the session is destroyed
    if (session_data.session)
    {
      session_data.session->FinishSession();
    }
  }

  return !gc_sessions_.empty();
}

bool OtlpHttpClient::IsShutdown() const noexcept
{
  return is_shutdown_.load(std::memory_order_acquire);
}

}  // namespace otlp
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE
