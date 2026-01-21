// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief #Azure::Core::Http::HttpTransport implementation via CURL.
 */

#pragma once

#include "azure/core/http/policies/policy.hpp"
#include "azure/core/http/transport.hpp"
#include "azure/core/nullable.hpp"

#include <chrono>
#include <memory>
#include <string>

namespace Azure { namespace Core { namespace Http {
  class CurlNetworkConnection;

  namespace _detail {
    /**
     * @brief Default maximum time in milliseconds that you allow the connection phase to the server
     * to take.
     *
     */
    constexpr std::chrono::milliseconds DefaultConnectionTimeout = std::chrono::minutes(5);
  } // namespace _detail

  /**
   * @brief The available options to set libcurl SSL options.
   *
   * @remark The SDK will map the enum option to libcurl's specific option. See more info here:
   * https://curl.se/libcurl/c/CURLOPT_SSL_OPTIONS.html
   *
   */
  struct CurlTransportSslOptions final
  {
    /**
     * @brief This option can enable the revocation list check.
     *
     * @remark Libcurl does revocation list check by default for SSL backends that supports this
     * feature. However, the Azure SDK overrides libcurl's behavior and disables the revocation list
     * check by default. This ensures that the libcurl behavior matches the WinHTTP behavior.
     */
    bool EnableCertificateRevocationListCheck = false;

    /**
     * @brief This option allows SSL connections to proceed even if there is an error retrieving the
     * Certificate Revocation List.
     *
     * @remark Note that this only works when libcurl is configured to use OpenSSL as its TLS
     * provider. That functionally limits this check to Linux only, and only when openssl is
     * configured (the default).
     */
    bool AllowFailedCrlRetrieval = false;

    /**
     * @brief A set of PEM encoded X.509 certificates and CRLs describing the certificates used to
     * validate the server.
     *
     * @remark The Azure SDK will not directly validate these certificates.
     *
     * @remark More about this option:
     * https://curl.se/libcurl/c/CURLOPT_CAINFO_BLOB.html
     *
     * @warning Requires libcurl >= 7.44.0
     *
     */
    std::string PemEncodedExpectedRootCertificates;
  };

  /**
   * @brief Set the libcurl connection options like a proxy and CA path.
   */
  struct CurlTransportOptions
  {
    /**
     * @brief The string for the proxy is passed directly to the libcurl handle without any parsing.
     *
     * @details libcurl will use system's environment proxy configuration (if it is set) when the \p
     * Proxy setting is not set (is null). Setting an empty string will make libcurl to ignore any
     * proxy settings from the system (use no proxy).
     *
     * @remark No validation for the string is done by the Azure SDK. More about this option:
     * https://curl.se/libcurl/c/CURLOPT_PROXY.html.
     *
     * @remark The default value is an empty string (no proxy).
     *
     */
    Azure::Nullable<std::string> Proxy;

    /**
     * @brief Username to be used for proxy connections.
     *
     * @remark No validation for the string is done by the Azure SDK. More about this option:
     * https://curl.se/libcurl/c/CURLOPT_PROXYUSERNAME.html.
     *
     * @remark The default value is an empty string (no proxy).
     *
     */
    Azure::Nullable<std::string> ProxyUsername;

    /**
     * @brief Password to be used for proxy connections.
     *
     * @remark No validation for the string is done by the Azure SDK. More about this option:
     * https://curl.se/libcurl/c/CURLOPT_PROXYPASSWORD.html.
     *
     * @remark If a value is provided, the value will be used (this allows the caller to provide an
     * empty password)
     *
     */
    Azure::Nullable<std::string> ProxyPassword;
    /**
     * @brief Path to a PEM encoded file containing the certificate authorities sent to libcurl
     * handle directly.
     *
     * @remark The Azure SDK will not check if the path is valid or not.
     *
     * @remark The default is the built-in system specific path. More about this option:
     * https://curl.se/libcurl/c/CURLOPT_CAINFO.html
     *
     * @remark This option is known to only work on Linux and might throw if set on other platforms.
     *
     */
    std::string CAInfo;

    /**
     * @brief Path to a directory which holds PEM encoded file, containing the certificate
     * authorities sent to libcurl handle directly.
     *
     * @remark The Azure SDK will not check if the path is valid or not.
     *
     * @remark The default is the built-in system specific path. More about this option:
     * https://curl.se/libcurl/c/CURLOPT_CAPATH.html
     *
     */
    std::string CAPath;

    /**
     * @brief All HTTP requests will keep the connection channel open to the service.
     *
     * @remark The channel might be closed by the server if the server response has an error code.
     * A connection won't be re-used if it is abandoned in the middle of an operation.
     * operation.
     *
     * @remark This option is managed directly by the Azure SDK. No option is set for the curl
     * handle. It is `true` by default.
     */
    bool HttpKeepAlive = true;

    /**
     * @brief This option determines whether libcurl verifies the authenticity of the peer's
     * certificate.
     *
     * @remark The default value is `true`. More about this option:
     * https://curl.se/libcurl/c/CURLOPT_SSL_VERIFYPEER.html
     *
     */
    bool SslVerifyPeer = true;

    /**
     * @brief Define the SSL options for the libcurl handle.
     *
     * @remark See more info here: https://curl.se/libcurl/c/CURLOPT_SSL_OPTIONS.html.
     * The default option is all options `false`.
     *
     */
    CurlTransportSslOptions SslOptions;

    /**
     * @brief When true, libcurl will not use any functions that install signal handlers or any
     * functions that cause signals to be sent to the process.
     *
     * @details This option is here to allow multi-threaded unix applications to still set/use all
     * timeout options etc, without risking getting signals.
     *
     */
    bool NoSignal = false;

    /**
     * @brief Contain the maximum time that you allow the connection phase to the server to take.
     *
     * @details This only limits the connection phase, it has no impact once it has connected.
     *
     * @remarks The default timeout is 300 seconds and using `0` would set this default value.
     *
     */
    std::chrono::milliseconds ConnectionTimeout = _detail::DefaultConnectionTimeout;

    /**
     * @brief If set, integrates libcurl's internal tracing with Azure logging.
     */
    bool EnableCurlTracing = false;
  };

  /**
   * @brief Concrete implementation of an HTTP Transport that uses libcurl.
   */
  class CurlTransport : public HttpTransport {
  private:
    CurlTransportOptions m_options;

    /**
     * @brief Called when an HTTP response indicates the connection should be upgraded to
     * a websocket. Takes ownership of the CurlNetworkConnection object.
     */
    virtual void OnUpgradedConnection(std::unique_ptr<CurlNetworkConnection>&&){};

  public:
    /**
     * @brief Construct a new CurlTransport object.
     *
     * @param options Optional parameter to override the default options.
     */
    CurlTransport(CurlTransportOptions const& options = CurlTransportOptions()) : m_options(options)
    {
    }

    /**
     * @brief Construct a new CurlTransport object based on common Azure HTTP Transport Options
     *
     * @param options Common Azure Core Transport Options.
     */
    CurlTransport(Azure::Core::Http::Policies::TransportOptions const& options);

    /**
     * @brief Destroys a CurlTransport object.
     *
     * See also:
     * [Core Guidelines C.35: "A base class destructor should be either public
     * and virtual or protected and
     * non-virtual"](http://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#c35-a-base-class-destructor-should-be-either-public-and-virtual-or-protected-and-non-virtual)
     */
    virtual ~CurlTransport() = default;

    /**
     * @brief Implements interface to send an HTTP Request and produce an HTTP RawResponse
     *
     * @param request an HTTP Request to be send.
     * @param context A context to control the request lifetime.
     *
     * @return unique ptr to an HTTP RawResponse.
     */
    std::unique_ptr<RawResponse> Send(Request& request, Context const& context) override;
  };

}}} // namespace Azure::Core::Http
