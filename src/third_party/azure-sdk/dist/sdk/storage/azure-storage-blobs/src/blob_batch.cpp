// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "azure/storage/blobs/blob_batch.hpp"

#include "private/package_version.hpp"

#include <azure/core/azure_assert.hpp>
#include <azure/core/http/policies/policy.hpp>
#include <azure/core/internal/http/pipeline.hpp>
#include <azure/core/io/body_stream.hpp>
#include <azure/storage/common/crypt.hpp>
#include <azure/storage/common/internal/constants.hpp>
#include <azure/storage/common/internal/shared_key_policy.hpp>

#include <algorithm>
#include <cstring>
#include <functional>
#include <future>
#include <stdexcept>

namespace Azure { namespace Storage { namespace Blobs {

  const Core::Context::Key _detail::s_serviceBatchKey;
  const Core::Context::Key _detail::s_containerBatchKey;

  namespace _detail {

    class BlobBatchAccessHelper final {
    public:
      explicit BlobBatchAccessHelper(const BlobServiceBatch& batch) : m_serviceBatch(&batch) {}
      explicit BlobBatchAccessHelper(const BlobContainerBatch& batch) : m_containerBatch(&batch) {}

      const std::vector<std::shared_ptr<BatchSubrequest>>& Subrequests() const
      {
        return m_serviceBatch ? m_serviceBatch->m_subrequests : m_containerBatch->m_subrequests;
      }

    private:
      const BlobServiceBatch* m_serviceBatch = nullptr;
      const BlobContainerBatch* m_containerBatch = nullptr;
    };

  } // namespace _detail

  namespace {
    const std::string LineEnding = "\r\n";
    const std::string BatchContentTypePrefix = "multipart/mixed; boundary=";

    static Core::Context::Key s_subrequestKey;
    static Core::Context::Key s_subresponseKey;

    struct Parser final
    {
      explicit Parser(const std::string& str)
          : startPos(str.data()), currPos(startPos), endPos(startPos + str.length())
      {
      }
      explicit Parser(const std::vector<uint8_t>& str)
          : startPos(reinterpret_cast<const char*>(str.data())),
            currPos(reinterpret_cast<const char*>(startPos)),
            endPos(reinterpret_cast<const char*>(startPos) + str.size())
      {
      }
      const char* startPos;
      const char* currPos;
      const char* endPos;

      bool IsEnd() const { return currPos == endPos; }

      bool LookAhead(const std::string& expect) const
      {
        for (size_t i = 0; i < expect.length(); ++i)
        {
          if (currPos + i < endPos && currPos[i] == expect[i])
          {
            continue;
          }
          return false;
        }
        return true;
      }

      void Consume(const std::string& expect)
      {
        // This moves currPos
        if (LookAhead(expect))
        {
          currPos += expect.length();
        }
        else
        {
          throw std::runtime_error(
              "failed to parse response body at " + std::to_string(currPos - startPos));
        }
      }

      const char* FindNext(const std::string& expect) const
      {
        return std::search(currPos, endPos, expect.begin(), expect.end());
      }

      const char* AfterNext(const std::string& expect) const
      {
        return (std::min)(endPos, FindNext(expect) + expect.length());
      }

      std::string GetBeforeNextAndConsume(const std::string& expect)
      {
        // This moves currPos
        auto ePos = FindNext(expect);
        std::string ret(currPos, ePos);
        currPos = (std::min)(endPos, ePos + expect.length());
        return ret;
      }
    };

    std::unique_ptr<Core::Http::RawResponse> ParseRawResponse(const std::string& responseText)
    {
      Parser parser(responseText);

      parser.Consume("HTTP/");
      int32_t httpMajorVersion = std::stoi(parser.GetBeforeNextAndConsume("."));
      int32_t httpMinorVersion = std::stoi(parser.GetBeforeNextAndConsume(" "));
      int32_t httpStatusCode = std::stoi(parser.GetBeforeNextAndConsume(" "));
      const std::string httpReasonPhrase = parser.GetBeforeNextAndConsume(LineEnding);

      auto rawResponse = std::make_unique<Azure::Core::Http::RawResponse>(
          httpMajorVersion,
          httpMinorVersion,
          static_cast<Azure::Core::Http::HttpStatusCode>(httpStatusCode),
          httpReasonPhrase);

      while (!parser.IsEnd())
      {
        if (parser.LookAhead(LineEnding))
        {
          break;
        }
        std::string headerName = parser.GetBeforeNextAndConsume(": ");
        std::string headerValue = parser.GetBeforeNextAndConsume(LineEnding);
        rawResponse->SetHeader(headerName, headerValue);
      }
      parser.Consume(LineEnding);
      rawResponse->SetBody(std::vector<uint8_t>(parser.currPos, parser.endPos));

      return rawResponse;
    }

    class RemoveXMsVersionPolicy final : public Core::Http::Policies::HttpPolicy {
    public:
      ~RemoveXMsVersionPolicy() override {}

      std::unique_ptr<HttpPolicy> Clone() const override
      {
        return std::make_unique<RemoveXMsVersionPolicy>(*this);
      }
      std::unique_ptr<Core::Http::RawResponse> Send(
          Core::Http::Request& request,
          Core::Http::Policies::NextHttpPolicy nextPolicy,
          const Core::Context& context) const override
      {
        request.RemoveHeader(_internal::HttpHeaderXMsVersion);
        return nextPolicy.Send(request, context);
      }
    };

    class NoopTransportPolicy final : public Core::Http::Policies::HttpPolicy {
    public:
      ~NoopTransportPolicy() override {}

      std::unique_ptr<HttpPolicy> Clone() const override
      {
        return std::make_unique<NoopTransportPolicy>(*this);
      }

      std::unique_ptr<Core::Http::RawResponse> Send(
          Core::Http::Request& request,
          Core::Http::Policies::NextHttpPolicy nextPolicy,
          const Core::Context& context) const override
      {
        (void)nextPolicy;

        std::string* subrequestText = nullptr;
        context.TryGetValue(s_subrequestKey, subrequestText);

        if (subrequestText)
        {
          std::string requestText = request.GetMethod().ToString() + " /"
              + request.GetUrl().GetRelativeUrl() + " HTTP/1.1" + LineEnding;
          for (const auto& header : request.GetHeaders())
          {
            requestText += header.first + ": " + header.second + LineEnding;
          }
          requestText += LineEnding;
          *subrequestText = std::move(requestText);

          auto rawResponse = std::make_unique<Core::Http::RawResponse>(
              1, 1, Core::Http::HttpStatusCode::Accepted, "Accepted");
          return rawResponse;
        }

        std::string* subresponseText = nullptr;
        context.TryGetValue(s_subresponseKey, subresponseText);
        if (subresponseText)
        {
          return ParseRawResponse(*subresponseText);
        }
        AZURE_UNREACHABLE_CODE();
      }
    };

    class ConstructBatchRequestBodyPolicy final : public Core::Http::Policies::HttpPolicy {
    public:
      ConstructBatchRequestBodyPolicy(
          std::function<void(Core::Http::Request&, const Core::Context&)> constructRequestFunction,
          std::function<void(std::unique_ptr<Core::Http::RawResponse>&, const Core::Context&)>
              parseResponseFunction)
          : m_constructRequestFunction(std::move(constructRequestFunction)),
            m_parseResponseFunction(std::move(parseResponseFunction))
      {
      }
      ~ConstructBatchRequestBodyPolicy() override {}

      std::unique_ptr<HttpPolicy> Clone() const override
      {
        return std::make_unique<ConstructBatchRequestBodyPolicy>(*this);
      }

      std::unique_ptr<Core::Http::RawResponse> Send(
          Core::Http::Request& request,
          Core::Http::Policies::NextHttpPolicy nextPolicy,
          const Core::Context& context) const override
      {
        m_constructRequestFunction(request, context);
        auto rawResponse = nextPolicy.Send(request, context);
        m_parseResponseFunction(rawResponse, context);
        return rawResponse;
      }

    private:
      std::function<void(Core::Http::Request&, const Core::Context&)> m_constructRequestFunction;
      std::function<void(std::unique_ptr<Core::Http::RawResponse>&, const Core::Context&)>
          m_parseResponseFunction;
    };

    template <class T>
    std::function<Response<T>()> CreateDeferredResponseFunc(
        std::promise<Nullable<Response<T>>>& promise)
    {
      return [&promise]() {
        try
        {
          auto f = promise.get_future();
          AZURE_ASSERT_MSG(
              f.wait_for(std::chrono::seconds(0)) == std::future_status::ready,
              "GetResponse() is called when response is not ready.");
          return f.get().Value();
        }
        catch (std::future_error&)
        {
          AZURE_ASSERT_MSG(false, "GetResponse() can only be called once.");
        }
        AZURE_UNREACHABLE_CODE();
      };
    }

    struct DeleteBlobSubrequest final : public _detail::BatchSubrequest
    {
      DeleteBlobSubrequest(Blobs::BlobClient blobClient, DeleteBlobOptions options)
          : BatchSubrequest(_detail::BatchSubrequestType::DeleteBlob),
            Client(std::move(blobClient)), Options(std::move(options))
      {
      }

      Blobs::BlobClient Client;
      DeleteBlobOptions Options;
      std::promise<Nullable<Response<Models::DeleteBlobResult>>> Promise;
    };

    struct SetBlobAccessTierSubrequest final : public _detail::BatchSubrequest
    {
      SetBlobAccessTierSubrequest(
          Blobs::BlobClient blobClient,
          Models::AccessTier tier,
          SetBlobAccessTierOptions options)
          : BatchSubrequest(_detail::BatchSubrequestType::SetBlobAccessTier),
            Client(std::move(blobClient)), Tier(std::move(tier)), Options(std::move(options))
      {
      }

      Blobs::BlobClient Client;
      Models::AccessTier Tier;
      SetBlobAccessTierOptions Options;
      std::promise<Nullable<Response<Models::SetBlobAccessTierResult>>> Promise;
    };

    void ConstructSubrequests(Core::Http::Request& request, const Core::Context& context)
    {
      const std::string boundary = "batch_" + Azure::Core::Uuid::CreateUuid().ToString();

      auto getBatchBoundary = [&boundary, subRequestCounter = 0]() mutable {
        std::string ret;
        ret += "--" + boundary + LineEnding;
        ret += "Content-Type: application/http" + LineEnding + "Content-Transfer-Encoding: binary"
            + LineEnding + "Content-ID: " + std::to_string(subRequestCounter++) + LineEnding
            + LineEnding;
        return ret;
      };

      std::string requestBody;

      std::unique_ptr<_detail::BlobBatchAccessHelper> batchAccessHelper;
      {
        const BlobServiceBatch* batch = nullptr;
        context.TryGetValue(_detail::s_serviceBatchKey, batch);
        if (batch)
        {
          batchAccessHelper = std::make_unique<_detail::BlobBatchAccessHelper>(*batch);
        }
      }
      {
        const BlobContainerBatch* batch = nullptr;
        context.TryGetValue(_detail::s_containerBatchKey, batch);
        if (batch)
        {
          batchAccessHelper = std::make_unique<_detail::BlobBatchAccessHelper>(*batch);
        }
      }

      for (const auto& subrequestPtr : batchAccessHelper->Subrequests())
      {
        if (subrequestPtr->Type == _detail::BatchSubrequestType::DeleteBlob)
        {
          auto& subrequest = *static_cast<DeleteBlobSubrequest*>(subrequestPtr.get());
          requestBody += getBatchBoundary();
          std::string subrequestText;
          subrequest.Client.Delete(
              subrequest.Options, Core::Context().WithValue(s_subrequestKey, &subrequestText));
          requestBody += subrequestText;
        }
        else if (subrequestPtr->Type == _detail::BatchSubrequestType::SetBlobAccessTier)
        {
          auto& subrequest = *static_cast<SetBlobAccessTierSubrequest*>(subrequestPtr.get());
          requestBody += getBatchBoundary();

          std::string subrequestText;
          subrequest.Client.SetAccessTier(
              subrequest.Tier,
              subrequest.Options,
              Core::Context().WithValue(s_subrequestKey, &subrequestText));
          requestBody += subrequestText;
        }
        else
        {
          AZURE_UNREACHABLE_CODE();
        }
      }
      requestBody += "--" + boundary + "--" + LineEnding;

      request.SetHeader(_internal::HttpHeaderContentType, BatchContentTypePrefix + boundary);
      static_cast<_detail::StringBodyStream&>(*request.GetBodyStream())
          = _detail::StringBodyStream(std::move(requestBody));
      request.SetHeader(
          _internal::HttpHeaderContentLength, std::to_string(request.GetBodyStream()->Length()));
    }

    void ParseSubresponses(
        std::unique_ptr<Core::Http::RawResponse>& rawResponse,
        const Core::Context& context)
    {
      if (rawResponse->GetStatusCode() != Core::Http::HttpStatusCode::Accepted
          || rawResponse->GetHeaders().count(_internal::HttpHeaderContentType) == 0)
      {
        return;
      }

      const std::string boundary = rawResponse->GetHeaders()
                                       .at(std::string(_internal::HttpHeaderContentType))
                                       .substr(BatchContentTypePrefix.length());

      const std::vector<uint8_t>& responseBody
          = rawResponse->ExtractBodyStream()->ReadToEnd(context);
      Parser parser(responseBody);

      std::vector<std::string> subresponses;
      while (true)
      {
        parser.Consume("--" + boundary);
        if (parser.LookAhead("--"))
        {
          parser.Consume("--");
        }
        if (parser.IsEnd())
        {
          break;
        }
        auto contentIdPos = parser.AfterNext("Content-ID: ");
        auto responseStartPos = parser.AfterNext(LineEnding + LineEnding);
        auto responseEndPos = parser.FindNext("--" + boundary);
        if (contentIdPos != parser.endPos)
        {
          parser.currPos = contentIdPos;
          auto idEndPos = parser.FindNext(LineEnding);
          size_t id = static_cast<size_t>(std::stoi(std::string(parser.currPos, idEndPos)));
          if (subresponses.size() < id + 1)
          {
            subresponses.resize(id + 1);
          }
          subresponses[id] = std::string(responseStartPos, responseEndPos);
          parser.currPos = responseEndPos;
        }
        else
        {
          rawResponse = ParseRawResponse(std::string(responseStartPos, responseEndPos));
          parser.currPos = responseEndPos;
          return;
        }
      }

      std::unique_ptr<_detail::BlobBatchAccessHelper> batchAccessHelper;
      {
        const BlobServiceBatch* batch = nullptr;
        context.TryGetValue(_detail::s_serviceBatchKey, batch);
        if (batch)
        {
          batchAccessHelper = std::make_unique<_detail::BlobBatchAccessHelper>(*batch);
        }
      }
      {
        const BlobContainerBatch* batch = nullptr;
        context.TryGetValue(_detail::s_containerBatchKey, batch);
        if (batch)
        {
          batchAccessHelper = std::make_unique<_detail::BlobBatchAccessHelper>(*batch);
        }
      }

      size_t subresponseCounter = 0;
      for (const auto& subrequestPtr : batchAccessHelper->Subrequests())
      {
        if (subrequestPtr->Type == _detail::BatchSubrequestType::DeleteBlob)
        {
          auto& subrequest = *static_cast<DeleteBlobSubrequest*>(subrequestPtr.get());
          try
          {
            auto response = subrequest.Client.Delete(
                subrequest.Options,
                Core::Context().WithValue(s_subresponseKey, &subresponses[subresponseCounter++]));
            subrequest.Promise.set_value(std::move(response));
          }
          catch (...)
          {
            subrequest.Promise.set_exception(std::current_exception());
          }
        }
        else if (subrequestPtr->Type == _detail::BatchSubrequestType::SetBlobAccessTier)
        {
          auto& subrequest = *static_cast<SetBlobAccessTierSubrequest*>(subrequestPtr.get());
          try
          {
            auto response = subrequest.Client.SetAccessTier(
                subrequest.Tier,
                subrequest.Options,
                Core::Context().WithValue(s_subresponseKey, &subresponses[subresponseCounter++]));
            subrequest.Promise.set_value(std::move(response));
          }
          catch (...)
          {
            subrequest.Promise.set_exception(std::current_exception());
          }
        }
        else
        {
          AZURE_UNREACHABLE_CODE();
        }
      }
    }
  } // namespace

  namespace _detail {

    size_t StringBodyStream::OnRead(
        uint8_t* buffer,
        size_t count,
        Azure::Core::Context const& context)
    {
      (void)context;
      size_t copy_length = (std::min)(count, m_content.length() - m_offset);
      std::memcpy(buffer, &m_content[0] + m_offset, static_cast<size_t>(copy_length));
      m_offset += copy_length;
      return copy_length;
    }

    BatchSubrequest::~BatchSubrequest() {}

    std::shared_ptr<Azure::Core::Http::_internal::HttpPipeline> ConstructBatchRequestPolicy(
        const std::vector<std::unique_ptr<Azure::Core::Http::Policies::HttpPolicy>>&
            servicePerRetryPolicies,
        const std::vector<std::unique_ptr<Azure::Core::Http::Policies::HttpPolicy>>&
            servicePerOperationPolicies,
        const BlobClientOptions& options)
    {
      std::vector<std::unique_ptr<Azure::Core::Http::Policies::HttpPolicy>> perRetryPolicies;
      perRetryPolicies.push_back(std::make_unique<ConstructBatchRequestBodyPolicy>(
          [](Core::Http::Request& request, const Core::Context& context) {
            ConstructSubrequests(request, context);
          },
          [](std::unique_ptr<Core::Http::RawResponse>& rawResponse, const Core::Context& context) {
            ParseSubresponses(rawResponse, context);
          }));
      for (auto& policy : servicePerRetryPolicies)
      {
        perRetryPolicies.push_back(policy->Clone());
      }
      std::vector<std::unique_ptr<Azure::Core::Http::Policies::HttpPolicy>> perOperationPolicies;
      for (auto& policy : servicePerOperationPolicies)
      {
        perOperationPolicies.push_back(policy->Clone());
      }
      return std::make_shared<Azure::Core::Http::_internal::HttpPipeline>(
          options,
          _internal::BlobServicePackageName,
          PackageVersion::ToString(),
          std::move(perRetryPolicies),
          std::move(perOperationPolicies));
    }

    std::shared_ptr<Azure::Core::Http::_internal::HttpPipeline> ConstructBatchSubrequestPolicy(
        std::unique_ptr<Azure::Core::Http::Policies::HttpPolicy>&& tokenAuthPolicy,
        std::unique_ptr<Azure::Core::Http::Policies::HttpPolicy>&& sharedKeyAuthPolicy,
        const BlobClientOptions& options)
    {
      std::vector<std::unique_ptr<Azure::Core::Http::Policies::HttpPolicy>> policies;
      policies.emplace_back(
          std::make_unique<Azure::Core::Http::Policies::_internal::RequestIdPolicy>());
      policies.emplace_back(
          std::make_unique<Azure::Core::Http::Policies::_internal::TelemetryPolicy>(
              _internal::BlobServicePackageName, PackageVersion::ToString(), options.Telemetry));
      for (auto& policy : options.PerOperationPolicies)
      {
        policies.emplace_back(policy->Clone());
      }
      policies.emplace_back(std::make_unique<_internal::StoragePerRetryPolicy>());
      if (tokenAuthPolicy)
      {
        policies.emplace_back(std::move(tokenAuthPolicy));
      }
      for (auto& policy : options.PerRetryPolicies)
      {
        policies.emplace_back(policy->Clone());
      }
      policies.emplace_back(std::make_unique<RemoveXMsVersionPolicy>());
      if (sharedKeyAuthPolicy)
      {
        policies.emplace_back(std::move(sharedKeyAuthPolicy));
      }
      policies.push_back(std::make_unique<NoopTransportPolicy>());
      return std::make_shared<Core::Http::_internal::HttpPipeline>(std::move(policies));
    }
  } // namespace _detail

  BlobServiceBatch::BlobServiceBatch(BlobServiceClient blobServiceClient)
      : m_blobServiceClient(std::move(blobServiceClient))
  {
  }

  BlobClient BlobServiceBatch::GetBlobClientForSubrequest(Core::Url url) const
  {
    auto blobClient = m_blobServiceClient.GetBlobContainerClient("$").GetBlobClient("$");
    blobClient.m_blobUrl = std::move(url);
    blobClient.m_pipeline = m_blobServiceClient.m_batchSubrequestPipeline;
    return blobClient;
  }

  DeferredResponse<Models::DeleteBlobResult> BlobServiceBatch::DeleteBlob(
      const std::string& blobContainerName,
      const std::string& blobName,
      const DeleteBlobOptions& options)
  {
    auto blobUrl = m_blobServiceClient.m_serviceUrl;
    blobUrl.AppendPath(_internal::UrlEncodePath(blobContainerName));
    blobUrl.AppendPath(_internal::UrlEncodePath(blobName));
    auto op = std::make_shared<DeleteBlobSubrequest>(
        GetBlobClientForSubrequest(std::move(blobUrl)), options);
    DeferredResponse<Models::DeleteBlobResult> deferredResponse(
        CreateDeferredResponseFunc(op->Promise));
    m_subrequests.push_back(std::move(op));
    return deferredResponse;
  }

  DeferredResponse<Models::DeleteBlobResult> BlobServiceBatch::DeleteBlobUrl(
      const std::string& blobUrl,
      const DeleteBlobOptions& options)
  {
    auto op = std::make_shared<DeleteBlobSubrequest>(
        GetBlobClientForSubrequest(Core::Url(blobUrl)), options);
    DeferredResponse<Models::DeleteBlobResult> deferredResponse(
        CreateDeferredResponseFunc(op->Promise));
    m_subrequests.push_back(std::move(op));
    return deferredResponse;
  }

  DeferredResponse<Models::SetBlobAccessTierResult> BlobServiceBatch::SetBlobAccessTier(
      const std::string& blobContainerName,
      const std::string& blobName,
      Models::AccessTier accessTier,
      const SetBlobAccessTierOptions& options)
  {
    auto blobUrl = m_blobServiceClient.m_serviceUrl;
    blobUrl.AppendPath(_internal::UrlEncodePath(blobContainerName));
    blobUrl.AppendPath(_internal::UrlEncodePath(blobName));
    auto op = std::make_shared<SetBlobAccessTierSubrequest>(
        GetBlobClientForSubrequest(std::move(blobUrl)), std::move(accessTier), options);
    DeferredResponse<Models::SetBlobAccessTierResult> deferredResponse(
        CreateDeferredResponseFunc(op->Promise));
    m_subrequests.push_back(std::move(op));
    return deferredResponse;
  }

  DeferredResponse<Models::SetBlobAccessTierResult> BlobServiceBatch::SetBlobAccessTierUrl(
      const std::string& blobUrl,
      Models::AccessTier accessTier,
      const SetBlobAccessTierOptions& options)
  {
    auto op = std::make_shared<SetBlobAccessTierSubrequest>(
        GetBlobClientForSubrequest(Core::Url(blobUrl)), std::move(accessTier), options);
    DeferredResponse<Models::SetBlobAccessTierResult> deferredResponse(
        CreateDeferredResponseFunc(op->Promise));
    m_subrequests.push_back(std::move(op));
    return deferredResponse;
  }

  BlobContainerBatch::BlobContainerBatch(BlobContainerClient blobContainerClient)
      : m_blobContainerClient(std::move(blobContainerClient))
  {
  }

  BlobClient BlobContainerBatch::GetBlobClientForSubrequest(Core::Url url) const
  {
    auto blobClient = m_blobContainerClient.GetBlobClient("$");
    blobClient.m_blobUrl = std::move(url);
    blobClient.m_pipeline = m_blobContainerClient.m_batchSubrequestPipeline;
    return blobClient;
  }

  DeferredResponse<Models::DeleteBlobResult> BlobContainerBatch::DeleteBlob(
      const std::string& blobName,
      const DeleteBlobOptions& options)
  {
    auto blobUrl = m_blobContainerClient.m_blobContainerUrl;
    blobUrl.AppendPath(_internal::UrlEncodePath(blobName));
    auto op = std::make_shared<DeleteBlobSubrequest>(
        GetBlobClientForSubrequest(std::move(blobUrl)), options);
    DeferredResponse<Models::DeleteBlobResult> deferredResponse(
        CreateDeferredResponseFunc(op->Promise));
    m_subrequests.push_back(std::move(op));
    return deferredResponse;
  }

  DeferredResponse<Models::DeleteBlobResult> BlobContainerBatch::DeleteBlobUrl(
      const std::string& blobUrl,
      const DeleteBlobOptions& options)
  {
    auto op = std::make_shared<DeleteBlobSubrequest>(
        GetBlobClientForSubrequest(Core::Url(blobUrl)), options);
    DeferredResponse<Models::DeleteBlobResult> deferredResponse(
        CreateDeferredResponseFunc(op->Promise));
    m_subrequests.push_back(std::move(op));
    return deferredResponse;
  }

  DeferredResponse<Models::SetBlobAccessTierResult> BlobContainerBatch::SetBlobAccessTier(
      const std::string& blobName,
      Models::AccessTier accessTier,
      const SetBlobAccessTierOptions& options)
  {
    auto blobUrl = m_blobContainerClient.m_blobContainerUrl;
    blobUrl.AppendPath(_internal::UrlEncodePath(blobName));
    auto op = std::make_shared<SetBlobAccessTierSubrequest>(
        GetBlobClientForSubrequest(std::move(blobUrl)), std::move(accessTier), options);
    DeferredResponse<Models::SetBlobAccessTierResult> deferredResponse(
        CreateDeferredResponseFunc(op->Promise));
    m_subrequests.push_back(std::move(op));
    return deferredResponse;
  }

  DeferredResponse<Models::SetBlobAccessTierResult> BlobContainerBatch::SetBlobAccessTierUrl(
      const std::string& blobUrl,
      Models::AccessTier accessTier,
      const SetBlobAccessTierOptions& options)
  {
    auto op = std::make_shared<SetBlobAccessTierSubrequest>(
        GetBlobClientForSubrequest(Core::Url(blobUrl)), std::move(accessTier), options);
    DeferredResponse<Models::SetBlobAccessTierResult> deferredResponse(
        CreateDeferredResponseFunc(op->Promise));
    m_subrequests.push_back(std::move(op));
    return deferredResponse;
  }
}}} // namespace Azure::Storage::Blobs
