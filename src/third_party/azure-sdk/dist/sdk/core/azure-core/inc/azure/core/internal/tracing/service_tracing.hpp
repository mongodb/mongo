// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "azure/core/context.hpp"
#include "azure/core/internal/client_options.hpp"
#include "azure/core/internal/extendable_enumeration.hpp"
#include "azure/core/internal/http/user_agent.hpp"
#include "azure/core/internal/tracing/tracing_impl.hpp"

#pragma once

/**
 *
 * @brief Helper classes to enable service client distributed tracing implementations.
 *
 */
namespace Azure { namespace Core { namespace Tracing { namespace _internal {

  /**
   * @brief RAII Helper class for Azure::Core::Tracing::Span objects.
   *
   * @details The ServiceSpan object is an RAII helper object used to manage Span objects.
   *
   * Before a Span is registered with OpenTelemetry, the span needs to have called the
   * "Azure::Core::Tracing::Span::End" method. The ServiceSpan method wraps an
   * Azure::Core::Tracing::Span object and ensures that the "End" method is called in the destructor
   * for the span.
   */
  class ServiceSpan final : public Span {
  private:
    std::shared_ptr<Span> m_span;

    friend class TracingContextFactory;
    ServiceSpan() = default;
    explicit ServiceSpan(std::shared_ptr<Span> span) : m_span(span) {}

    ServiceSpan(const ServiceSpan&) = delete;
    ServiceSpan& operator=(ServiceSpan const&) = delete;

    ServiceSpan& operator=(ServiceSpan&&) noexcept = default;

  public:
    ServiceSpan(ServiceSpan&& that) = default;

    ~ServiceSpan()
    {
      if (m_span)
      {
        m_span->End({});
      }
    }

    void End() { End({}); }

    void End(Azure::Nullable<Azure::DateTime> endTime) override
    {
      if (m_span)
      {
        m_span->End(endTime);
      }
    }
    void SetStatus(Azure::Core::Tracing::_internal::SpanStatus const& status)
    {
      SetStatus(status, {});
    }
    void SetStatus(
        Azure::Core::Tracing::_internal::SpanStatus const& status,
        std::string const& description) override
    {
      if (m_span)
      {
        m_span->SetStatus(status, description);
      }
    }

    /**
     * @brief Adds a set of attributes to the span.
     *
     * @param attributeToAdd Attributes to be added to the span.
     */
    void AddAttributes(AttributeSet const& attributeToAdd) override
    {
      if (m_span)
      {
        m_span->AddAttributes(attributeToAdd);
      }
    }

    /**
     * @brief Adds a single attributes to the span.
     *
     * @param attributeName Name of the attribute to be added.
     * @param attributeValue Value of the attribute to be added.
     */
    void AddAttribute(std::string const& attributeName, std::string const& attributeValue) override
    {
      if (m_span)
      {
        m_span->AddAttribute(attributeName, attributeValue);
      }
    }

    /**
     * @brief Adds an event to the span.
     *
     * Add an Event to the span. An event is identified by a name and an optional set of attributes
     * associated with the event.
     *
     * @param eventName Name of the event to add.
     * @param eventAttributes Attributes associated with the event.
     */
    void AddEvent(std::string const& eventName, AttributeSet const& eventAttributes) override
    {
      if (m_span)
      {
        m_span->AddEvent(eventName, eventAttributes);
      }
    }

    /**
     * @brief Adds an event to the span.
     *
     * Add an Event to the span. An event is identified by a name
     *
     * @param eventName Name of the event to add.
     */
    void AddEvent(std::string const& eventName) override
    {
      if (m_span)
      {
        m_span->AddEvent(eventName);
      }
    }

    /**
     * @brief Records an exception occurring in the span. Also marks the status of the span as
     * SpanStatus::Error
     *
     * @param exception Exception which has occurred.
     */
    void AddEvent(std::exception const& exception) override
    {
      if (m_span)
      {
        m_span->AddEvent(exception);
        m_span->SetStatus(SpanStatus::Error, {});
      }
    }

    /**
     * @brief Propagate information from the current span to the HTTP request headers.
     *
     * @param request HTTP Request to the service. If there is an active tracing span, this will
     * add required headers to the HTTP Request.
     */
    void PropagateToHttpHeaders(Azure::Core::Http::Request& request) override
    {
      if (m_span)
      {
        m_span->PropagateToHttpHeaders(request);
      }
    }
  };

  /**
   * @brief Helper class to enable distributed tracing for the service.
   *
   * @details Each service implementation SHOULD have a member variable which aids in managing
   * the distributed tracing for the service.
   */
  class TracingContextFactory final {
  private:
    std::string m_serviceName;
    std::string m_packageName;
    std::string m_packageVersion;
    std::shared_ptr<Azure::Core::Tracing::_internal::Tracer> m_serviceTracer;

    /** @brief The key used to retrieve the span and tracer associated with a context object.
     *
     *  The value stored in the context with this key is a `std::pair<std::shared_ptr<Span>,
     * std::shared_ptr<Tracer>>`.
     *
     * A caller can use the Span and Tracer to create a new span associated with the current
     * context span.
     */
    static Azure::Core::Context::Key ContextSpanKey;
    static Azure::Core::Context::Key TracingFactoryContextKey;

  public:
    /**
     * @brief Construct a new Tracing Context Factory object
     *
     * @param options Client Options for tracing.
     * @param serviceName Name of the resource provider for the service [See
     * also](https://docs.microsoft.com/azure/azure-resource-manager/management/azure-services-resource-providers).
     * @param packageName Name of the package containing this service client.
     * @param packageVersion Optional package version number for the package containing this
     * service. (https://opentelemetry.io/docs/reference/specification/trace/api/#get-a-tracer).
     */
    TracingContextFactory(
        Azure::Core::_internal::ClientOptions const& options,
        std::string const& serviceName,
        std::string const& packageName,
        std::string packageVersion)
        : m_serviceName{serviceName}, m_packageName{packageName}, m_packageVersion{packageVersion}
    {
      // If the caller has configured a tracing provider, use it. Otherwise, use the default
      // provider.
      if (options.Telemetry.TracingProvider)
      {
        m_serviceTracer
            = Azure::Core::Tracing::_internal::TracerProviderImplGetter::TracerImplFromTracer(
                  options.Telemetry.TracingProvider)
                  ->CreateTracer(packageName, packageVersion);
      }
    }
    /**
     * @brief Construct a new Tracing Context Factory object
     *
     * @param options Client Options for tracing.
     * @param serviceName Name of the resource provider for the service [See
     * also](https://docs.microsoft.com/azure/azure-resource-manager/management/azure-services-resource-providers).
     * @param packageVersion Package version number for the package containing this
     * service. (https://opentelemetry.io/docs/reference/specification/trace/api/#get-a-tracer).
     *
     * @deprecated This constructor is deprecated and should not be used by any service code. It
     * exists only to support an earlier release of the Attestation SDK and should not be used by
     * any code beyond that.
     */
    [[deprecated]] TracingContextFactory(
        Azure::Core::_internal::ClientOptions const& options,
        std::string const& serviceName,
        std::string packageVersion)
        : TracingContextFactory(options, serviceName, serviceName, packageVersion)
    {
    }

    TracingContextFactory() = default;
    TracingContextFactory(TracingContextFactory const&) = default;

    /** @brief A TracingContext provides an updated Context object and a new span object
     * which can be used to add events and attributes to the span.
     */
    struct TracingContext
    {
      /**
       * @brief New Context to be used for subsequent methods which take a Context parameter.
       */
      Azure::Core::Context Context;
      /**
       * @brief Distributed Tracing Span which can be used to update status if the API succeeds or
       * fails.
       */
      ServiceSpan Span;
    };

    /**
     * @brief Create a span with the specified span name.
     *
     * @details This method is a convenience method intended for use by service clients, it creates
     * a SpanKind::Internal span and context.
     *
     * @param spanName Name for the span to be created.
     * @param context parent context object for the newly created span.
     *
     * @returns Newly allocated context and Span object.
     *
     */
    TracingContext CreateTracingContext(
        std::string const& spanName,
        Azure::Core::Context const& context) const;

    /**
     * @brief Create a span with the specified span name and create options.
     *
     * @param spanName Name for the span to be created.
     * @param spanOptions Options for the newly created span.
     * @param context parent context object for the newly created span.
     *
     * @returns Newly allocated context and Span object.
     *
     */
    TracingContext CreateTracingContext(
        std::string const& spanName,
        Azure::Core::Tracing::_internal::CreateSpanOptions& spanOptions,
        Azure::Core::Context const& context) const;

    std::unique_ptr<Azure::Core::Tracing::_internal::AttributeSet> CreateAttributeSet() const;

    /** @brief Returns true if this TracingContextFactory is connected to a service tracer.
     */
    bool HasTracer() const { return static_cast<bool>(m_serviceTracer); }

    static std::unique_ptr<TracingContextFactory> CreateFromContext(
        Azure::Core::Context const& context);
  };

  /**
   * @brief Attributes emitted as a part of distributed tracing spans.
   *
   * List taken from here:
   * https://github.com/Azure/azure-sdk/blob/main/docs/tracing/distributed-tracing-conventions.yml
   *
   */
  class TracingAttributes
      : public Azure::Core::_internal::ExtendableEnumeration<TracingAttributes> {
  public:
    explicit TracingAttributes(std::string const& that) : ExtendableEnumeration(that) {}

    /**
     * @brief
     * [Namespace](https://docs.microsoft.com/azure/azure-resource-manager/management/azure-services-resource-providers)
     * of Azure service request is made against.
     *
     * @remarks Azure Specific attribute.
     *
     */
    AZ_CORE_DLLEXPORT const static TracingAttributes AzNamespace;

    /** @brief  Value of the[x - ms - client - request - id] header(or other request - id header,
     * depending on the service) sent by the client.
     *
     * @remarks Azure Specific attribute.
     */
    AZ_CORE_DLLEXPORT const static TracingAttributes RequestId;

    /** @brief Value of the [x-ms-request-id]  header (or other request-id header, depending on the
     * service) sent by the server in response.
     *
     * @remarks Azure Specific attribute.
     */
    AZ_CORE_DLLEXPORT const static TracingAttributes ServiceRequestId;

    /**
     * @brief HTTP request method.
     *
     */
    AZ_CORE_DLLEXPORT const static TracingAttributes HttpMethod;

    /**
     * @brief Full HTTP request URL in the form `scheme://host[:port]/path?query[#fragment]`.
     *
     */
    AZ_CORE_DLLEXPORT const static TracingAttributes HttpUrl;

    /**
     * @brief [HTTP response status code](https://tools.ietf.org/html/rfc7231#section-6).
     *
     */
    AZ_CORE_DLLEXPORT const static TracingAttributes HttpStatusCode;

    /**
     * @brief Value of the [HTTP User-Agent](https://tools.ietf.org/html/rfc7231#section-5.5.3)
     * header sent by the client.
     *
     */
    AZ_CORE_DLLEXPORT const static TracingAttributes HttpUserAgent;

    /** @brief Fully qualified Azure service endpoint(host name component)
     *
     * For example: 'http://my-account.servicebus.windows.net/'
     */
    AZ_CORE_DLLEXPORT const static TracingAttributes NetPeerName;

    /** @brief Port of the Azure Service Endpoint
     *
     * For example: 'http://my-account.servicebus.windows.net/'
     */
    AZ_CORE_DLLEXPORT const static TracingAttributes NetPeerPort;
  };

}}}} // namespace Azure::Core::Tracing::_internal
