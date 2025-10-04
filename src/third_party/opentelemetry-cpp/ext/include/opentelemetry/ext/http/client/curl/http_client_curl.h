// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <curl/curl.h>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "opentelemetry/ext/http/client/curl/http_operation_curl.h"
#include "opentelemetry/ext/http/client/http_client.h"
#include "opentelemetry/nostd/function_ref.h"
#include "opentelemetry/nostd/shared_ptr.h"
#include "opentelemetry/nostd/string_view.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace ext
{
namespace http
{
namespace client
{
namespace curl
{

const opentelemetry::ext::http::client::StatusCode Http_Ok = 200;

class HttpCurlGlobalInitializer
{
private:
  HttpCurlGlobalInitializer(const HttpCurlGlobalInitializer &) = delete;
  HttpCurlGlobalInitializer(HttpCurlGlobalInitializer &&)      = delete;

  HttpCurlGlobalInitializer &operator=(const HttpCurlGlobalInitializer &) = delete;

  HttpCurlGlobalInitializer &operator=(HttpCurlGlobalInitializer &&) = delete;

  HttpCurlGlobalInitializer();

public:
  ~HttpCurlGlobalInitializer();

  static nostd::shared_ptr<HttpCurlGlobalInitializer> GetInstance();
};

class Request : public opentelemetry::ext::http::client::Request
{
public:
  Request() : method_(opentelemetry::ext::http::client::Method::Get), uri_("/") {}

  void SetMethod(opentelemetry::ext::http::client::Method method) noexcept override
  {
    method_ = method;
  }

  void SetSslOptions(const HttpSslOptions &ssl_options) noexcept override
  {
    ssl_options_ = ssl_options;
  }

  void SetBody(opentelemetry::ext::http::client::Body &body) noexcept override
  {
    body_ = std::move(body);
  }

  void AddHeader(nostd::string_view name, nostd::string_view value) noexcept override
  {
    headers_.insert(std::pair<std::string, std::string>(static_cast<std::string>(name),
                                                        static_cast<std::string>(value)));
  }

  void ReplaceHeader(nostd::string_view name, nostd::string_view value) noexcept override
  {
    // erase matching headers
    auto range = headers_.equal_range(static_cast<std::string>(name));
    headers_.erase(range.first, range.second);
    AddHeader(name, value);
  }

  void SetUri(nostd::string_view uri) noexcept override { uri_ = static_cast<std::string>(uri); }

  void SetTimeoutMs(std::chrono::milliseconds timeout_ms) noexcept override
  {
    timeout_ms_ = timeout_ms;
  }

  void SetCompression(
      const opentelemetry::ext::http::client::Compression &compression) noexcept override
  {
    compression_ = compression;
  }

public:
  opentelemetry::ext::http::client::Method method_;
  opentelemetry::ext::http::client::HttpSslOptions ssl_options_;
  opentelemetry::ext::http::client::Body body_;
  opentelemetry::ext::http::client::Headers headers_;
  std::string uri_;
  std::chrono::milliseconds timeout_ms_{5000};  // ms
  opentelemetry::ext::http::client::Compression compression_{
      opentelemetry::ext::http::client::Compression::kNone};
};

class Response : public opentelemetry::ext::http::client::Response
{
public:
  Response() : status_code_(Http_Ok) {}

  const opentelemetry::ext::http::client::Body &GetBody() const noexcept override { return body_; }

  bool ForEachHeader(nostd::function_ref<bool(nostd::string_view name, nostd::string_view value)>
                         callable) const noexcept override
  {
    for (const auto &header : headers_)
    {
      if (!callable(header.first, header.second))
      {
        return false;
      }
    }
    return true;
  }

  bool ForEachHeader(const nostd::string_view &name,
                     nostd::function_ref<bool(nostd::string_view name, nostd::string_view value)>
                         callable) const noexcept override
  {
    auto range = headers_.equal_range(static_cast<std::string>(name));
    for (auto it = range.first; it != range.second; ++it)
    {
      if (!callable(it->first, it->second))
      {
        return false;
      }
    }
    return true;
  }

  opentelemetry::ext::http::client::StatusCode GetStatusCode() const noexcept override
  {
    return status_code_;
  }

public:
  Headers headers_;
  opentelemetry::ext::http::client::Body body_;
  opentelemetry::ext::http::client::StatusCode status_code_;
};

class HttpClient;

class Session : public opentelemetry::ext::http::client::Session,
                public std::enable_shared_from_this<Session>
{
public:
  Session(HttpClient &http_client,
          std::string scheme      = "http",
          const std::string &host = "",
          uint16_t port           = 80)
      : http_client_(http_client)
  {
    host_ = scheme + "://" + host + ":" + std::to_string(port) + "/";
  }

  std::shared_ptr<opentelemetry::ext::http::client::Request> CreateRequest() noexcept override
  {
    http_request_.reset(new Request());
    return http_request_;
  }

  void SendRequest(
      std::shared_ptr<opentelemetry::ext::http::client::EventHandler> callback) noexcept override;

  bool CancelSession() noexcept override;

  bool FinishSession() noexcept override;

  bool IsSessionActive() noexcept override
  {
    return is_session_active_.load(std::memory_order_acquire);
  }

  void SetId(uint64_t session_id) { session_id_ = session_id; }

  /**
   * Returns the base URI.
   * @return the base URI as a string consisting of scheme, host and port.
   */
  const std::string &GetBaseUri() const { return host_; }

  std::shared_ptr<Request> GetRequest() { return http_request_; }

  inline HttpClient &GetHttpClient() noexcept { return http_client_; }
  inline const HttpClient &GetHttpClient() const noexcept { return http_client_; }

  inline uint64_t GetSessionId() const noexcept { return session_id_; }

  inline const std::unique_ptr<HttpOperation> &GetOperation() const noexcept
  {
    return curl_operation_;
  }
  inline std::unique_ptr<HttpOperation> &GetOperation() noexcept { return curl_operation_; }

  /**
   * Finish and cleanup the operation.It will remove curl easy handle in it from HttpClient
   */
  void FinishOperation();

private:
  std::shared_ptr<Request> http_request_;
  std::string host_;
  std::unique_ptr<HttpOperation> curl_operation_;
  uint64_t session_id_;
  HttpClient &http_client_;
  std::atomic<bool> is_session_active_{false};
};

class HttpClientSync : public opentelemetry::ext::http::client::HttpClientSync
{
public:
  HttpClientSync() : curl_global_initializer_(HttpCurlGlobalInitializer::GetInstance()) {}

  opentelemetry::ext::http::client::Result Get(
      const nostd::string_view &url,
      const opentelemetry::ext::http::client::HttpSslOptions &ssl_options,
      const opentelemetry::ext::http::client::Headers &headers,
      const opentelemetry::ext::http::client::Compression &compression) noexcept override
  {
    opentelemetry::ext::http::client::Body body;

    HttpOperation curl_operation(opentelemetry::ext::http::client::Method::Get, url.data(),
                                 ssl_options, nullptr, headers, body, compression);

    curl_operation.SendSync();
    auto session_state = curl_operation.GetSessionState();
    if (curl_operation.WasAborted())
    {
      session_state = opentelemetry::ext::http::client::SessionState::Cancelled;
    }
    auto response = std::unique_ptr<Response>(new Response());
    if (curl_operation.GetResponseCode() >= CURL_LAST)
    {
      // we have a http response

      response->headers_     = curl_operation.GetResponseHeaders();
      response->body_        = curl_operation.GetResponseBody();
      response->status_code_ = curl_operation.GetResponseCode();
    }
    return opentelemetry::ext::http::client::Result(std::move(response), session_state);
  }

  opentelemetry::ext::http::client::Result Post(
      const nostd::string_view &url,
      const opentelemetry::ext::http::client::HttpSslOptions &ssl_options,
      const Body &body,
      const opentelemetry::ext::http::client::Headers &headers,
      const opentelemetry::ext::http::client::Compression &compression) noexcept override
  {
    HttpOperation curl_operation(opentelemetry::ext::http::client::Method::Post, url.data(),
                                 ssl_options, nullptr, headers, body, compression);
    curl_operation.SendSync();
    auto session_state = curl_operation.GetSessionState();
    if (curl_operation.WasAborted())
    {
      session_state = opentelemetry::ext::http::client::SessionState::Cancelled;
    }
    auto response = std::unique_ptr<Response>(new Response());
    if (curl_operation.GetResponseCode() >= CURL_LAST)
    {
      // we have a http response

      response->headers_     = curl_operation.GetResponseHeaders();
      response->body_        = curl_operation.GetResponseBody();
      response->status_code_ = curl_operation.GetResponseCode();
    }

    return opentelemetry::ext::http::client::Result(std::move(response), session_state);
  }

public:
  ~HttpClientSync() override {}

private:
  nostd::shared_ptr<HttpCurlGlobalInitializer> curl_global_initializer_;
};

class HttpClient : public opentelemetry::ext::http::client::HttpClient
{
public:
  // The call (curl_global_init) is not thread safe. Ensure this is called only once.
  HttpClient();
  ~HttpClient() override;

  std::shared_ptr<opentelemetry::ext::http::client::Session> CreateSession(
      nostd::string_view url) noexcept override;

  bool CancelAllSessions() noexcept override;

  bool FinishAllSessions() noexcept override;

  void SetMaxSessionsPerConnection(std::size_t max_requests_per_connection) noexcept override;

  inline uint64_t GetMaxSessionsPerConnection() const noexcept
  {
    return max_sessions_per_connection_;
  }

  void CleanupSession(uint64_t session_id);

  inline CURLM *GetMultiHandle() noexcept { return multi_handle_; }

  void MaybeSpawnBackgroundThread();

  void ScheduleAddSession(uint64_t session_id);
  void ScheduleAbortSession(uint64_t session_id);
  void ScheduleRemoveSession(uint64_t session_id, HttpCurlEasyResource &&resource);

  void WaitBackgroundThreadExit()
  {
    std::unique_ptr<std::thread> background_thread;
    {
      std::lock_guard<std::mutex> lock_guard{background_thread_m_};
      background_thread.swap(background_thread_);
    }

    if (background_thread && background_thread->joinable())
    {
      background_thread->join();
    }
  }

private:
  void wakeupBackgroundThread();
  bool doAddSessions();
  bool doAbortSessions();
  bool doRemoveSessions();
  void resetMultiHandle();

  std::mutex multi_handle_m_;
  CURLM *multi_handle_;
  std::atomic<uint64_t> next_session_id_{0};
  uint64_t max_sessions_per_connection_;

  std::mutex sessions_m_;
  std::recursive_mutex session_ids_m_;
  std::unordered_map<uint64_t, std::shared_ptr<Session>> sessions_;
  std::unordered_set<uint64_t> pending_to_add_session_ids_;
  std::unordered_map<uint64_t, std::shared_ptr<Session>> pending_to_abort_sessions_;
  std::unordered_map<uint64_t, HttpCurlEasyResource> pending_to_remove_session_handles_;
  std::list<std::shared_ptr<Session>> pending_to_remove_sessions_;

  std::mutex background_thread_m_;
  std::unique_ptr<std::thread> background_thread_;
  std::chrono::milliseconds scheduled_delay_milliseconds_;

  nostd::shared_ptr<HttpCurlGlobalInitializer> curl_global_initializer_;
};

}  // namespace curl
}  // namespace client
}  // namespace http
}  // namespace ext
OPENTELEMETRY_END_NAMESPACE
