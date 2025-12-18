// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "opentelemetry/nostd/function_ref.h"
#include "opentelemetry/nostd/string_view.h"
#include "opentelemetry/version.h"

/*
 Usage Example

Sync Request:

  HttpClient httpClient;
  auto result = httpClient.Get(url); // GET request
  if (result){
    auto response = result.GetResponse();
  } else {
    std::cout << result.GetSessionState();
  }

Async Request:

 struct SimpleReponseHandler: public ResponseHandler {
      void OnResponse(Response& res) noexcept override
      {
           if (res.IsSuccess()) {
            res.GetNextHeader([](nostd::string_view name, std::string value) -> bool {
                std::cout << "Header Name:" << name << " Header Value:"<< value ;
                return true;
            });
            .. process response body
           }
      }

      void OnError(nostd::string_view err) noexcept override
      {
          std::cerr << " Error:" << err;
      }
  };

  HttpClient httpClient; // implementer can provide singleton implementation for it
  auto session = httpClient.createSession("localhost" + 8000);
  auto request = session->CreateRequest();
  request->AddHeader(..);
  SimpleResponseHandler res_handler;
  session->SendRequest(res_handler);
  session->FinishSession() // optionally in the end
  ...shutdown
  httpClient.FinishAllSessions()

*/

OPENTELEMETRY_BEGIN_NAMESPACE
namespace ext
{
namespace http
{
namespace client
{

enum class Method
{
  Get,
  Post,
  Put,
  Options,
  Head,
  Patch,
  Delete
};

enum class SessionState
{
  CreateFailed,        // session create failed
  Created,             // session created
  Destroyed,           // session destroyed
  Connecting,          // connecting to peer
  ConnectFailed,       // connection failed
  Connected,           // connected
  Sending,             // sending request
  SendFailed,          // request send failed
  Response,            // response received
  SSLHandshakeFailed,  // SSL handshake failed
  TimedOut,            // request time out
  NetworkError,        // network error
  ReadError,           // error reading response
  WriteError,          // error writing request
  Cancelled            // (manually) cancelled
};

enum class Compression
{
  kNone,
  kGzip
};

using Byte       = uint8_t;
using StatusCode = uint16_t;
using Body       = std::vector<Byte>;

struct cmp_ic
{
  bool operator()(const std::string &s1, const std::string &s2) const
  {
    return std::lexicographical_compare(
        s1.begin(), s1.end(), s2.begin(), s2.end(),
        [](char c1, char c2) { return ::tolower(c1) < ::tolower(c2); });
  }
};
using Headers = std::multimap<std::string, std::string, cmp_ic>;

struct HttpSslOptions
{
  HttpSslOptions() {}

  HttpSslOptions(nostd::string_view url,
                 bool input_ssl_insecure_skip_verify,
                 nostd::string_view input_ssl_ca_cert_path,
                 nostd::string_view input_ssl_ca_cert_string,
                 nostd::string_view input_ssl_client_key_path,
                 nostd::string_view input_ssl_client_key_string,
                 nostd::string_view input_ssl_client_cert_path,
                 nostd::string_view input_ssl_client_cert_string,
                 nostd::string_view input_ssl_min_tls,
                 nostd::string_view input_ssl_max_tls,
                 nostd::string_view input_ssl_cipher,
                 nostd::string_view input_ssl_cipher_suite)
      : use_ssl(false),
        ssl_insecure_skip_verify(input_ssl_insecure_skip_verify),
        ssl_ca_cert_path(input_ssl_ca_cert_path),
        ssl_ca_cert_string(input_ssl_ca_cert_string),
        ssl_client_key_path(input_ssl_client_key_path),
        ssl_client_key_string(input_ssl_client_key_string),
        ssl_client_cert_path(input_ssl_client_cert_path),
        ssl_client_cert_string(input_ssl_client_cert_string),
        ssl_min_tls(input_ssl_min_tls),
        ssl_max_tls(input_ssl_max_tls),
        ssl_cipher(input_ssl_cipher),
        ssl_cipher_suite(input_ssl_cipher_suite)
  {
    /* Use SSL if url starts with "https:" */
    if (strncmp(url.data(), "https:", 6) == 0)
    {
      use_ssl = true;
    }
  }

  /**
    Use HTTPS (true) or HTTP (false).
  */
  bool use_ssl{false};
  /**
    Skip SSL/TLS verifications.
    Setting this flag to true is insecure.
  */
  bool ssl_insecure_skip_verify{false};
  /**
    Path to the CA CERT file.
  */
  std::string ssl_ca_cert_path{};
  /**
    CA CERT.
    Used only if @p ssl_ca_cert_path is empty.
  */
  std::string ssl_ca_cert_string{};
  /**
    Path to the client key file.
  */
  std::string ssl_client_key_path{};
  /**
    Client key.
    Used only if @p ssl_client_key_path is empty.
  */
  std::string ssl_client_key_string{};
  /**
    Path to the client cert file.
  */
  std::string ssl_client_cert_path{};
  /**
    Client cert.
    Used only if @p ssl_client_cert_path is empty.
  */
  std::string ssl_client_cert_string{};

  /**
    Minimum SSL version to use.
    Valid values are:
    - empty (defaults to TLSv1.2)
    - "1.2" (TLSv1.2)
    - "1.3" (TLSv1.3)
  */
  std::string ssl_min_tls{};

  /**
    Maximum SSL version to use.
    Valid values are:
    - empty (no maximum version required)
    - "1.2" (TLSv1.2)
    - "1.3" (TLSv1.3)
  */
  std::string ssl_max_tls{};

  /**
    TLS Cipher.
    This is for TLS 1.2.
    The list is delimited by colons (":").
    Cipher names depends on the underlying CURL implementation.
  */
  std::string ssl_cipher{};

  /**
    TLS Cipher suite.
    This is for TLS 1.3.
    The list is delimited by colons (":").
    Cipher names depends on the underlying CURL implementation.
  */
  std::string ssl_cipher_suite{};
};

using SecondsDecimal = std::chrono::duration<float, std::ratio<1>>;

struct RetryPolicy
{
  RetryPolicy() = default;

  RetryPolicy(std::uint32_t input_max_attempts,
              SecondsDecimal input_initial_backoff,
              SecondsDecimal input_max_backoff,
              float input_backoff_multiplier)
      : max_attempts(input_max_attempts),
        initial_backoff(input_initial_backoff),
        max_backoff(input_max_backoff),
        backoff_multiplier(input_backoff_multiplier)
  {}

  std::uint32_t max_attempts{};
  SecondsDecimal initial_backoff{};
  SecondsDecimal max_backoff{};
  float backoff_multiplier{};
};

class Request
{
public:
  virtual void SetMethod(Method method) noexcept = 0;

  virtual void SetUri(nostd::string_view uri) noexcept = 0;

  virtual void SetSslOptions(const HttpSslOptions &options) noexcept = 0;

  virtual void SetBody(Body &body) noexcept = 0;

  virtual void AddHeader(nostd::string_view name, nostd::string_view value) noexcept = 0;

  virtual void ReplaceHeader(nostd::string_view name, nostd::string_view value) noexcept = 0;

  virtual void SetTimeoutMs(std::chrono::milliseconds timeout_ms) noexcept = 0;

  virtual void SetCompression(const Compression &compression) noexcept = 0;

  virtual void EnableLogging(bool is_log_enabled) noexcept = 0;

  virtual void SetRetryPolicy(const RetryPolicy &retry_policy) noexcept = 0;

  virtual ~Request() = default;
};

class Response
{
public:
  virtual const Body &GetBody() const noexcept = 0;

  virtual bool ForEachHeader(
      nostd::function_ref<bool(nostd::string_view name, nostd::string_view value)> callable)
      const noexcept = 0;

  virtual bool ForEachHeader(
      const nostd::string_view &key,
      nostd::function_ref<bool(nostd::string_view name, nostd::string_view value)> callable)
      const noexcept = 0;

  virtual StatusCode GetStatusCode() const noexcept = 0;

  virtual ~Response() = default;
};

class NoopResponse : public Response
{
public:
  const Body &GetBody() const noexcept override
  {
    static Body body;
    return body;
  }
  bool ForEachHeader(nostd::function_ref<bool(nostd::string_view name, nostd::string_view value)>
                     /* callable */) const noexcept override
  {
    return true;
  }

  bool ForEachHeader(const nostd::string_view & /* key */,
                     nostd::function_ref<bool(nostd::string_view name, nostd::string_view value)>
                     /* callable */) const noexcept override
  {
    return true;
  }

  StatusCode GetStatusCode() const noexcept override { return 0; }
};

class Result
{

public:
  Result(std::unique_ptr<Response> res, SessionState session_state)
      : response_(std::move(res)), session_state_(session_state)
  {}
  operator bool() const { return session_state_ == SessionState::Response; }
  Response &GetResponse()
  {
    if (response_ == nullptr)
    {
      // let's not return nullptr
      response_.reset(new NoopResponse());
    }
    return *response_;
  }
  SessionState GetSessionState() { return session_state_; }

private:
  std::unique_ptr<Response> response_;
  SessionState session_state_;
};

class EventHandler
{
public:
  virtual void OnResponse(Response &) noexcept = 0;

  virtual void OnEvent(SessionState, nostd::string_view) noexcept = 0;

  virtual ~EventHandler() = default;
};

class Session
{
public:
  virtual std::shared_ptr<Request> CreateRequest() noexcept = 0;

  virtual void SendRequest(std::shared_ptr<EventHandler>) noexcept = 0;

  virtual bool IsSessionActive() noexcept = 0;

  virtual bool CancelSession() noexcept = 0;

  virtual bool FinishSession() noexcept = 0;

  virtual ~Session() = default;
};

class HttpClient
{
public:
  virtual std::shared_ptr<Session> CreateSession(nostd::string_view url) noexcept = 0;

  virtual bool CancelAllSessions() noexcept = 0;

  virtual bool FinishAllSessions() noexcept = 0;

  virtual void SetMaxSessionsPerConnection(std::size_t max_requests_per_connection) noexcept = 0;

  virtual ~HttpClient() = default;
};

class HttpClientSync
{
public:
  Result GetNoSsl(const nostd::string_view &url,
                  const Headers &headers         = {{}},
                  const Compression &compression = Compression::kNone) noexcept
  {
    static const HttpSslOptions no_ssl;
    return Get(url, no_ssl, headers, compression);
  }

  virtual Result PostNoSsl(const nostd::string_view &url,
                           const Body &body,
                           const Headers &headers         = {{"content-type", "application/json"}},
                           const Compression &compression = Compression::kNone) noexcept
  {
    static const HttpSslOptions no_ssl;
    return Post(url, no_ssl, body, headers, compression);
  }

  virtual Result Get(const nostd::string_view &url,
                     const HttpSslOptions &ssl_options,
                     const Headers                & = {{}},
                     const Compression &compression = Compression::kNone) noexcept = 0;

  virtual Result Post(const nostd::string_view &url,
                      const HttpSslOptions &ssl_options,
                      const Body &body,
                      const Headers                & = {{"content-type", "application/json"}},
                      const Compression &compression = Compression::kNone) noexcept = 0;

  virtual ~HttpClientSync() = default;
};

}  // namespace client
}  // namespace http
}  // namespace ext
OPENTELEMETRY_END_NAMESPACE
