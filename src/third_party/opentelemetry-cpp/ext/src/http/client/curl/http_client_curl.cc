// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <curl/curl.h>
#include <curl/curlver.h>
#include <zconf.h>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "opentelemetry/ext/http/client/curl/http_client_curl.h"
#include "opentelemetry/ext/http/client/curl/http_operation_curl.h"
#include "opentelemetry/ext/http/client/http_client.h"
#include "opentelemetry/ext/http/common/url_parser.h"
#include "opentelemetry/nostd/shared_ptr.h"
#include "opentelemetry/nostd/string_view.h"
#include "opentelemetry/version.h"

#ifdef ENABLE_OTLP_COMPRESSION_PREVIEW
#  include <zlib.h>
#else
#  include "opentelemetry/sdk/common/global_log_handler.h"
#endif

OPENTELEMETRY_BEGIN_NAMESPACE
namespace ext
{
namespace http
{
namespace client
{
namespace curl
{

HttpCurlGlobalInitializer::HttpCurlGlobalInitializer()
{
  curl_global_init(CURL_GLOBAL_ALL);
}

HttpCurlGlobalInitializer::~HttpCurlGlobalInitializer()
{
  curl_global_cleanup();
}

nostd::shared_ptr<HttpCurlGlobalInitializer> HttpCurlGlobalInitializer::GetInstance()
{
  static nostd::shared_ptr<HttpCurlGlobalInitializer> shared_initializer{
      new HttpCurlGlobalInitializer()};
  return shared_initializer;
}

void Session::SendRequest(
    std::shared_ptr<opentelemetry::ext::http::client::EventHandler> callback) noexcept
{
  is_session_active_.store(true, std::memory_order_release);
  std::string url       = host_ + std::string(http_request_->uri_);
  auto callback_ptr     = callback.get();
  bool reuse_connection = false;

  // Set CURLOPT_FRESH_CONNECT and CURLOPT_FORBID_REUSE to 1L every max_sessions_per_connection_
  // requests. So libcurl will create a new connection instead of queue the request to the existing
  // connection.
  if (http_client_.GetMaxSessionsPerConnection() > 0)
  {
    reuse_connection = session_id_ % http_client_.GetMaxSessionsPerConnection() != 0;
  }

  if (http_request_->compression_ == opentelemetry::ext::http::client::Compression::kGzip)
  {
#ifdef ENABLE_OTLP_COMPRESSION_PREVIEW
    http_request_->AddHeader("Content-Encoding", "gzip");

    opentelemetry::ext::http::client::Body compressed_body(http_request_->body_.size());
    z_stream zs;
    zs.zalloc    = Z_NULL;
    zs.zfree     = Z_NULL;
    zs.opaque    = Z_NULL;
    zs.avail_in  = static_cast<uInt>(http_request_->body_.size());
    zs.next_in   = http_request_->body_.data();
    zs.avail_out = static_cast<uInt>(compressed_body.size());
    zs.next_out  = compressed_body.data();

    // ZLIB: Have to maually specify 16 bits for the Gzip headers
    const int window_bits = 15 + 16;

    int stream =
        deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, window_bits, 8, Z_DEFAULT_STRATEGY);

    if (stream == Z_OK)
    {
      deflate(&zs, Z_FINISH);
      deflateEnd(&zs);
      compressed_body.resize(zs.total_out);
      http_request_->SetBody(compressed_body);
    }
    else
    {
      if (callback)
      {
        callback->OnEvent(opentelemetry::ext::http::client::SessionState::CreateFailed, "");
      }
      is_session_active_.store(false, std::memory_order_release);
    }
#else
    OTEL_INTERNAL_LOG_ERROR(
        "[HTTP Client Curl] Set WITH_OTLP_HTTP_COMPRESSION=ON to use gzip compression with the "
        "OTLP HTTP Exporter");
#endif
  }

  curl_operation_.reset(new HttpOperation(http_request_->method_, url, http_request_->ssl_options_,
                                          callback_ptr, http_request_->headers_,
                                          http_request_->body_, http_request_->compression_, false,
                                          http_request_->timeout_ms_, reuse_connection));
  bool success =
      CURLE_OK == curl_operation_->SendAsync(this, [this, callback](HttpOperation &operation) {
        if (operation.WasAborted())
        {
          // Manually cancelled
          callback->OnEvent(opentelemetry::ext::http::client::SessionState::Cancelled, "");
        }

        if (operation.GetSessionState() == opentelemetry::ext::http::client::SessionState::Response)
        {
          // we have a http response
          auto response          = std::unique_ptr<Response>(new Response());
          response->headers_     = operation.GetResponseHeaders();
          response->body_        = operation.GetResponseBody();
          response->status_code_ = operation.GetResponseCode();
          callback->OnResponse(*response);
        }
        is_session_active_.store(false, std::memory_order_release);
      });

  if (success)
  {
    // We will try to create a background to poll events.But when the background is running, we will
    // reuse it instead of creating a new one.
    http_client_.MaybeSpawnBackgroundThread();
  }
  else
  {
    if (callback)
    {
      callback->OnEvent(opentelemetry::ext::http::client::SessionState::CreateFailed, "");
    }
    is_session_active_.store(false, std::memory_order_release);
  }
}

bool Session::CancelSession() noexcept
{
  if (curl_operation_)
  {
    curl_operation_->Abort();
  }
  http_client_.CleanupSession(session_id_);
  return true;
}

bool Session::FinishSession() noexcept
{
  if (curl_operation_)
  {
    curl_operation_->Finish();
  }
  http_client_.CleanupSession(session_id_);
  return true;
}

void Session::FinishOperation()
{
  if (curl_operation_)
  {
    curl_operation_->Cleanup();
  }
}

HttpClient::HttpClient()
    : multi_handle_(curl_multi_init()),
      next_session_id_{0},
      max_sessions_per_connection_{8},
      scheduled_delay_milliseconds_{std::chrono::milliseconds(256)},
      curl_global_initializer_(HttpCurlGlobalInitializer::GetInstance())
{}

HttpClient::~HttpClient()
{
  while (true)
  {
    std::unique_ptr<std::thread> background_thread;
    {
      std::lock_guard<std::mutex> lock_guard{background_thread_m_};
      background_thread.swap(background_thread_);
    }

    // Force to abort all sessions
    CancelAllSessions();

    if (!background_thread)
    {
      break;
    }
    if (background_thread->joinable())
    {
      background_thread->join();
    }
  }
  {
    std::lock_guard<std::mutex> lock_guard{multi_handle_m_};
    curl_multi_cleanup(multi_handle_);
  }
}

std::shared_ptr<opentelemetry::ext::http::client::Session> HttpClient::CreateSession(
    nostd::string_view url) noexcept
{
  auto parsedUrl = common::UrlParser(std::string(url));
  if (!parsedUrl.success_)
  {
    return std::make_shared<Session>(*this);
  }
  auto session =
      std::make_shared<Session>(*this, parsedUrl.scheme_, parsedUrl.host_, parsedUrl.port_);
  auto session_id = ++next_session_id_;
  session->SetId(session_id);

  std::lock_guard<std::mutex> lock_guard{sessions_m_};
  sessions_.insert({session_id, session});

  // FIXME: Session may leak if it do not call SendRequest
  return session;
}

bool HttpClient::CancelAllSessions() noexcept
{
  // CancelSession may change sessions_, we can not change a container while iterating it.
  while (true)
  {
    std::unordered_map<uint64_t, std::shared_ptr<Session>> sessions;
    {
      // We can only cleanup session and curl handles in the IO thread.
      std::lock_guard<std::mutex> lock_guard{sessions_m_};
      sessions = sessions_;
    }

    if (sessions.empty())
    {
      break;
    }

    for (auto &session : sessions)
    {
      session.second->CancelSession();
    }
  }
  return true;
}

bool HttpClient::FinishAllSessions() noexcept
{
  // FinishSession may change sessions_, we can not change a container while iterating it.
  while (true)
  {
    std::unordered_map<uint64_t, std::shared_ptr<Session>> sessions;
    {
      // We can only cleanup session and curl handles in the IO thread.
      std::lock_guard<std::mutex> lock_guard{sessions_m_};
      sessions = sessions_;
    }

    if (sessions.empty())
    {
      break;
    }

    for (auto &session : sessions)
    {
      session.second->FinishSession();
    }
  }
  return true;
}

void HttpClient::SetMaxSessionsPerConnection(std::size_t max_requests_per_connection) noexcept
{
  max_sessions_per_connection_ = max_requests_per_connection;
}

void HttpClient::CleanupSession(uint64_t session_id)
{
  std::shared_ptr<Session> session;
  {
    std::lock_guard<std::mutex> lock_guard{sessions_m_};
    auto it = sessions_.find(session_id);
    if (it != sessions_.end())
    {
      session = it->second;
      sessions_.erase(it);
    }
  }

  bool need_wakeup_background_thread = false;
  {
    std::lock_guard<std::recursive_mutex> lock_guard{session_ids_m_};
    pending_to_add_session_ids_.erase(session_id);

    if (session)
    {
      if (pending_to_remove_session_handles_.end() !=
          pending_to_remove_session_handles_.find(session_id))
      {
        pending_to_remove_sessions_.emplace_back(std::move(session));
      }
      else if (session->IsSessionActive() && session->GetOperation())
      {
        // If this session is already running, give it to the background thread for cleanup.
        pending_to_abort_sessions_[session_id] = std::move(session);
        need_wakeup_background_thread          = true;
      }
    }
  }

  if (need_wakeup_background_thread)
  {
    wakeupBackgroundThread();
  }
}

void HttpClient::MaybeSpawnBackgroundThread()
{
  std::lock_guard<std::mutex> lock_guard{background_thread_m_};
  if (background_thread_)
  {
    return;
  }

  background_thread_.reset(new std::thread(
      [](HttpClient *self) {
        int still_running = 1;
        while (true)
        {
          CURLMsg *msg;
          int queued;
          CURLMcode mc = curl_multi_perform(self->multi_handle_, &still_running);
          // According to https://curl.se/libcurl/c/curl_multi_perform.html, when mc is not OK, we
          // can not curl_multi_perform it again
          if (mc != CURLM_OK)
          {
            self->resetMultiHandle();
          }
          else if (still_running)
          {
        // curl_multi_poll is added from libcurl 7.66.0, before 7.68.0, we can only wait util
        // timeout to do the rest jobs
#if LIBCURL_VERSION_NUM >= 0x074200
            /* wait for activity, timeout or "nothing" */
            mc = curl_multi_poll(self->multi_handle_, nullptr, 0,
                                 static_cast<int>(self->scheduled_delay_milliseconds_.count()),
                                 nullptr);
#else
            mc = curl_multi_wait(self->multi_handle_, nullptr, 0,
                                 static_cast<int>(self->scheduled_delay_milliseconds_.count()),
                                 nullptr);
#endif
          }

          do
          {
            msg = curl_multi_info_read(self->multi_handle_, &queued);
            if (msg == nullptr)
            {
              break;
            }

            if (msg->msg == CURLMSG_DONE)
            {
              CURL *easy_handle = msg->easy_handle;
              CURLcode result   = msg->data.result;
              Session *session  = nullptr;
              curl_easy_getinfo(easy_handle, CURLINFO_PRIVATE, &session);
              // If it's already moved into pending_to_remove_session_handles_, we just ingore this
              // message.
              if (nullptr != session && session->GetOperation())
              {
                // Session can not be destroyed when calling PerformCurlMessage
                auto hold_session = session->shared_from_this();
                session->GetOperation()->PerformCurlMessage(result);
              }
            }
          } while (true);

          // Abort all pending easy handles
          if (self->doAbortSessions())
          {
            still_running = 1;
          }

          // Remove all pending easy handles
          if (self->doRemoveSessions())
          {
            still_running = 1;
          }

          // Add all pending easy handles
          if (self->doAddSessions())
          {
            still_running = 1;
          }

          if (still_running == 0)
          {
            std::lock_guard<std::mutex> lock_guard{self->background_thread_m_};
            // Double check, make sure no more pending sessions after locking background thread
            // management

            // Abort all pending easy handles
            if (self->doAbortSessions())
            {
              still_running = 1;
            }

            // Remove all pending easy handles
            if (self->doRemoveSessions())
            {
              still_running = 1;
            }

            // Add all pending easy handles
            if (self->doAddSessions())
            {
              still_running = 1;
            }

            // If there is no pending jobs, we can stop the background thread.
            if (still_running == 0)
            {
              if (self->background_thread_)
              {
                self->background_thread_->detach();
                self->background_thread_.reset();
              }
              break;
            }
          }
        }
      },
      this));
}

void HttpClient::ScheduleAddSession(uint64_t session_id)
{
  {
    std::lock_guard<std::recursive_mutex> lock_guard{session_ids_m_};
    pending_to_add_session_ids_.insert(session_id);
    pending_to_remove_session_handles_.erase(session_id);
    pending_to_abort_sessions_.erase(session_id);
  }

  wakeupBackgroundThread();
}

void HttpClient::ScheduleAbortSession(uint64_t session_id)
{
  {
    std::lock_guard<std::mutex> lock_guard{sessions_m_};
    auto session = sessions_.find(session_id);
    if (session == sessions_.end())
    {
      std::lock_guard<std::recursive_mutex> lock_guard{session_ids_m_};
      pending_to_add_session_ids_.erase(session_id);
    }
    else
    {
      std::lock_guard<std::recursive_mutex> lock_guard{session_ids_m_};
      pending_to_abort_sessions_[session_id] = std::move(session->second);
      pending_to_add_session_ids_.erase(session_id);

      sessions_.erase(session);
    }
  }

  wakeupBackgroundThread();
}

void HttpClient::ScheduleRemoveSession(uint64_t session_id, HttpCurlEasyResource &&resource)
{
  {
    std::lock_guard<std::recursive_mutex> lock_guard{session_ids_m_};
    pending_to_add_session_ids_.erase(session_id);
    pending_to_remove_session_handles_[session_id] = std::move(resource);
  }

  wakeupBackgroundThread();
}

void HttpClient::wakeupBackgroundThread()
{
// Before libcurl 7.68.0, we can only wait for timeout and do the rest jobs
// See https://curl.se/libcurl/c/curl_multi_wakeup.html
#if LIBCURL_VERSION_NUM >= 0x074400
  std::lock_guard<std::mutex> lock_guard{multi_handle_m_};
  if (nullptr != multi_handle_)
  {
    curl_multi_wakeup(multi_handle_);
  }
#endif
}

bool HttpClient::doAddSessions()
{
  std::unordered_set<uint64_t> pending_to_add_session_ids;
  {
    std::lock_guard<std::recursive_mutex> session_id_lock_guard{session_ids_m_};
    pending_to_add_session_ids_.swap(pending_to_add_session_ids);
  }

  bool has_data = false;

  std::lock_guard<std::mutex> lock_guard{sessions_m_};
  for (auto &session_id : pending_to_add_session_ids)
  {
    auto session = sessions_.find(session_id);
    if (session == sessions_.end())
    {
      continue;
    }

    if (!session->second->GetOperation())
    {
      continue;
    }

    CURL *easy_handle = session->second->GetOperation()->GetCurlEasyHandle();
    if (nullptr == easy_handle)
    {
      continue;
    }

    curl_multi_add_handle(multi_handle_, easy_handle);
    has_data = true;
  }

  return has_data;
}

bool HttpClient::doAbortSessions()
{
  std::unordered_map<uint64_t, std::shared_ptr<Session>> pending_to_abort_sessions;
  {
    std::lock_guard<std::recursive_mutex> session_id_lock_guard{session_ids_m_};
    pending_to_abort_sessions_.swap(pending_to_abort_sessions);
  }

  bool has_data = false;
  for (const auto &session : pending_to_abort_sessions)
  {
    if (!session.second)
    {
      continue;
    }

    if (session.second->GetOperation())
    {
      session.second->FinishOperation();
      has_data = true;
    }
  }
  return has_data;
}

bool HttpClient::doRemoveSessions()
{
  bool has_data = false;
  bool should_continue;
  do
  {
    std::unordered_map<uint64_t, HttpCurlEasyResource> pending_to_remove_session_handles;
    std::list<std::shared_ptr<Session>> pending_to_remove_sessions;
    {
      std::lock_guard<std::recursive_mutex> session_id_lock_guard{session_ids_m_};
      pending_to_remove_session_handles_.swap(pending_to_remove_session_handles);
      pending_to_remove_sessions_.swap(pending_to_remove_sessions);
    }
    {
      // If user callback do not call CancelSession or FinishSession, We still need to remove it
      // from sessions_
      std::lock_guard<std::mutex> session_lock_guard{sessions_m_};
      for (auto &removing_handle : pending_to_remove_session_handles)
      {
        auto session = sessions_.find(removing_handle.first);
        if (session != sessions_.end())
        {
          pending_to_remove_sessions.emplace_back(std::move(session->second));
          sessions_.erase(session);
        }
      }
    }

    for (auto &removing_handle : pending_to_remove_session_handles)
    {
      if (nullptr != removing_handle.second.headers_chunk)
      {
        curl_slist_free_all(removing_handle.second.headers_chunk);
      }

      curl_multi_remove_handle(multi_handle_, removing_handle.second.easy_handle);
      curl_easy_cleanup(removing_handle.second.easy_handle);
    }

    for (auto &removing_session : pending_to_remove_sessions)
    {
      // This operation may add more pending_to_remove_session_handles
      removing_session->FinishOperation();
    }

    should_continue =
        !pending_to_remove_session_handles.empty() || !pending_to_remove_sessions.empty();
    if (should_continue)
    {
      has_data = true;
    }
  } while (should_continue);

  return has_data;
}

void HttpClient::resetMultiHandle()
{
  std::list<std::shared_ptr<Session>> sessions;
  std::lock_guard<std::mutex> session_lock_guard{sessions_m_};
  {
    std::lock_guard<std::recursive_mutex> session_id_lock_guard{session_ids_m_};
    for (auto &session : sessions_)
    {
      if (pending_to_add_session_ids_.end() == pending_to_add_session_ids_.find(session.first))
      {
        sessions.push_back(session.second);
      }
    }
  }

  for (auto &session : sessions)
  {
    session->CancelSession();
    session->FinishOperation();
  }

  doRemoveSessions();

  // We will modify the multi_handle_, so we need to lock it
  std::lock_guard<std::mutex> lock_guard{multi_handle_m_};
  curl_multi_cleanup(multi_handle_);

  // Create a another multi handle to continue pending sessions
  multi_handle_ = curl_multi_init();
}

}  // namespace curl
}  // namespace client
}  // namespace http
}  // namespace ext
OPENTELEMETRY_END_NAMESPACE
