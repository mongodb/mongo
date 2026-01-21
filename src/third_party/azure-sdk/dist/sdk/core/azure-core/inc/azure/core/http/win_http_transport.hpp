// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief #Azure::Core::Http::HttpTransport implementation via WinHTTP.
 */

#pragma once

#include "azure/core/context.hpp"
#include "azure/core/http/http.hpp"
#include "azure/core/http/policies/policy.hpp"
#include "azure/core/http/transport.hpp"
#include "azure/core/internal/unique_handle.hpp"
#include "azure/core/nullable.hpp"
#include "azure/core/url.hpp"

#include <memory>
#include <string>
#include <vector>

namespace Azure { namespace Core { namespace Http {
  namespace _detail {
    class WinHttpRequest;
  }

  /**
   * @brief Sets the WinHTTP session and connection options used to customize the behavior of the
   * transport.
   */
  struct WinHttpTransportOptions final
  {
    /**
     * @brief When `true`, allows an invalid certificate authority.
     */
    bool IgnoreUnknownCertificateAuthority{false};

    /**
     * @brief When `true`, allows an invalid common name in a certificate.
     */
    bool IgnoreInvalidCertificateCommonName{false};

    /**
     * Proxy information.
     */

    /**
     * @brief If True, enables the use of the system default proxy.
     *
     * @remarks Set this to "true" if you would like to use a local HTTP proxy like "Fiddler" to
     * capture and analyze HTTP traffic.
     *
     * Set to "false" by default because it is not recommended to use a proxy for production and
     * Fiddler's proxy interferes with the HTTP functional tests.
     */
    bool EnableSystemDefaultProxy{false};

    /**
     * @brief If True, enables checks for certificate revocation.
     */
    bool EnableCertificateRevocationListCheck{false};

    /**
     * @brief Proxy information.
     *
     * @remark The Proxy Information string is composed of a set of elements
     * formatted as follows:
     *
     * @code
     * (\[\<scheme\>=\]\[\<scheme\>"://"\]\<server\>\[":"\<port\>\])
     * @endcode
     *
     * Each element should be separated with semicolons or whitespace.
     */
    std::string ProxyInformation;

    /**
     * @brief User name for proxy authentication.
     */
    Azure::Nullable<std::string> ProxyUserName;

    /**
     * @brief Password for proxy authentication.
     */
    Azure::Nullable<std::string> ProxyPassword;

    /**
     * @brief Array of Base64 encoded DER encoded X.509 certificate.  These certificates should
     * form a chain of certificates which will be used to validate the server certificate sent by
     * the server.
     */
    std::vector<std::string> ExpectedTlsRootCertificates;
  };

  /**
   * @brief Concrete implementation of an HTTP transport that uses WinHTTP when sending and
   * receiving requests and responses over the wire.
   */
  class WinHttpTransport : public HttpTransport {
  private:
    WinHttpTransportOptions m_options;
    // m_sessionhandle is const to ensure immutability.
    const Azure::Core::_internal::UniqueHandle<void*> m_sessionHandle;

    Azure::Core::_internal::UniqueHandle<void*> CreateSessionHandle();
    Azure::Core::_internal::UniqueHandle<void*> CreateConnectionHandle(
        Azure::Core::Url const& url,
        Azure::Core::Context const& context);

    std::unique_ptr<_detail::WinHttpRequest> CreateRequestHandle(
        Azure::Core::_internal::UniqueHandle<void*> const& connectionHandle,
        Azure::Core::Url const& url,
        Azure::Core::Http::HttpMethod const& method);

    // Callback to allow a derived transport to extract the request handle. Used for WebSocket
    // transports.
    virtual void OnUpgradedConnection(std::unique_ptr<_detail::WinHttpRequest> const&){};

  public:
    /**
     * @brief Constructs `%WinHttpTransport`.
     *
     * @param options Optional parameter to override the default settings.
     */
    WinHttpTransport(WinHttpTransportOptions const& options = WinHttpTransportOptions());

    /**
     * @brief Constructs `%WinHttpTransport`.
     *
     * @param options Optional parameter to override the default settings.
     */
    /**
     * @brief Constructs `%WinHttpTransport` object based on common Azure HTTP Transport Options
     *
     */
    WinHttpTransport(Azure::Core::Http::Policies::TransportOptions const& options);

    /**
     * @brief Implements the HTTP transport interface to send an HTTP Request and produce an
     * HTTP RawResponse.
     *
     */
    virtual std::unique_ptr<RawResponse> Send(Request& request, Context const& context) override;

    // See also:
    // [Core Guidelines C.35: "A base class destructor should be either public
    // and virtual or protected and
    // non-virtual"](http://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#c35-a-base-class-destructor-should-be-either-public-and-virtual-or-protected-and-non-virtual)
    virtual ~WinHttpTransport();
  };

}}} // namespace Azure::Core::Http
