// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
// cspell:words HCERTIFICATECHAIN PCCERT CCERT HCERTCHAINENGINE HCERTSTORE lpsz REFERER

#include "azure/core/base64.hpp"
#include "azure/core/diagnostics/logger.hpp"
#include "azure/core/http/http.hpp"
#include "azure/core/internal/diagnostics/log.hpp"
#include "azure/core/internal/strings.hpp"
#include "azure/core/internal/unique_handle.hpp"

#if defined(BUILD_TRANSPORT_WINHTTP_ADAPTER)
#include "azure/core/http/win_http_transport.hpp"
#include "win_http_request.hpp"
#endif

#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#define NOMINMAX
#endif

#include <Windows.h>

#include <algorithm>
#include <sstream>
#include <string>
#include <type_traits>
#pragma warning(push)
#pragma warning(disable : 6553)
#pragma warning(disable : 6387) // An argument in result_macros.h may be '0', for the function
                                // 'GetProcAddress'.
#include <wil/resource.h> // definitions for wil::unique_cert_chain_context and other RAII type wrappers for Windows types.
#pragma warning(pop)

#include <wincrypt.h>
#include <winhttp.h>

// cspell: ignore hcertstore

using Azure::Core::Context;
using namespace Azure::Core::Http;
using namespace Azure::Core::Diagnostics;
using namespace Azure::Core::Diagnostics::_internal;

namespace {

constexpr static const size_t DefaultUploadChunkSize = 1024 * 64;
constexpr static const size_t MaximumUploadChunkSize = 1024 * 1024;

std::string GetErrorMessage(DWORD error)
{
  std::string errorMessage = " Error Code: " + std::to_string(error);

  char* errorMsg = nullptr;
  if (FormatMessageA(
          FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_ALLOCATE_BUFFER,
          GetModuleHandleA("winhttp.dll"),
          error,
          MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
          reinterpret_cast<LPSTR>(&errorMsg),
          0,
          nullptr)
      != 0)
  {
    // Use a unique_ptr to manage the lifetime of errorMsg.
    std::unique_ptr<char, decltype(&LocalFree)> errorString(errorMsg, &LocalFree);
    errorMsg = nullptr;

    errorMessage += ": ";
    errorMessage += errorString.get();
    // If the end of the error message is a CRLF, remove it.
    if (errorMessage.back() == '\n')
    {
      errorMessage.erase(errorMessage.size() - 1);
      if (errorMessage.back() == '\r')
      {
        errorMessage.erase(errorMessage.size() - 1);
      }
    }
  }
  errorMessage += '.';
  return errorMessage;
}

void GetErrorAndThrow(const std::string& exceptionMessage, DWORD error = GetLastError())
{
  throw Azure::Core::Http::TransportException(exceptionMessage + GetErrorMessage(error));
}

const std::string HttpScheme = "http";
const std::string WebSocketScheme = "ws";

inline std::wstring HttpMethodToWideString(HttpMethod method)
{
  // This string should be all uppercase.
  // Many servers treat HTTP verbs as case-sensitive, and the Internet Engineering Task Force (IETF)
  // Requests for Comments (RFCs) spell these verbs using uppercase characters only.

  std::string httpMethodString = method.ToString();

  // Assuming ASCII here is OK since the input is expected to be an HTTP method string.
  // Converting this way is only safe when the text is ASCII.
  std::wstring wideStr(httpMethodString.begin(), httpMethodString.end());
  return wideStr;
}

// Convert a UTF-8 string to a wide Unicode string.
// This assumes the input string is always null-terminated.
std::wstring StringToWideString(const std::string& str)
{
  // Since the strings being converted to wstring can be provided by the end user, and can contain
  // invalid characters, use the MB_ERR_INVALID_CHARS to validate and fail.

  // Passing in -1 so that the function processes the entire input string, including the terminating
  // null character.
  int sizeNeeded = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, str.c_str(), -1, 0, 0);
  if (sizeNeeded == 0)
  {
    // Errors include:
    // ERROR_INSUFFICIENT_BUFFER
    // ERROR_INVALID_FLAGS
    // ERROR_INVALID_PARAMETER
    // ERROR_NO_UNICODE_TRANSLATION
    DWORD error = GetLastError();
    throw Azure::Core::Http::TransportException(
        "Unable to get the required transcoded size for the input string. Error Code: "
        + std::to_string(error) + ".");
  }

  std::wstring wideStr(sizeNeeded, L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, str.c_str(), -1, &wideStr[0], sizeNeeded)
      == 0)
  {
    DWORD error = GetLastError();
    throw Azure::Core::Http::TransportException(
        "Unable to transcode the input string to a wide string. Error Code: "
        + std::to_string(error) + ".");
  }
  return wideStr;
}

// Convert a wide Unicode string to a UTF-8 string.
std::string WideStringToString(const std::wstring& wideString)
{
  // We can't always assume the input wide string is null-terminated, so need to pass in the actual
  // size.
  size_t wideStrSize = wideString.size();
  if (wideStrSize > INT_MAX)
  {
    throw Azure::Core::Http::TransportException(
        "Input wide string is too large to fit within a 32-bit int.");
  }

  // Note, we are not using the flag WC_ERR_INVALID_CHARS here, because it is assumed the service
  // returns correctly encoded response headers and reason phrase strings.
  // The transport layer shouldn't do additional validation, and if WideCharToMultiByte replaces
  // invalid characters with the replacement character, that is fine.

  int wideStrLength = static_cast<int>(wideStrSize);
  int sizeNeeded
      = WideCharToMultiByte(CP_UTF8, 0, wideString.c_str(), wideStrLength, NULL, 0, NULL, NULL);
  if (sizeNeeded == 0)
  {
    // Errors include:
    // ERROR_INSUFFICIENT_BUFFER
    // ERROR_INVALID_FLAGS
    // ERROR_INVALID_PARAMETER
    // ERROR_NO_UNICODE_TRANSLATION
    DWORD error = GetLastError();
    throw Azure::Core::Http::TransportException(
        "Unable to get the required transcoded size for the input wide string. Error Code: "
        + std::to_string(error) + ".");
  }

  std::string str(sizeNeeded, 0);
  if (WideCharToMultiByte(
          CP_UTF8, 0, wideString.c_str(), wideStrLength, &str[0], sizeNeeded, NULL, NULL)
      == 0)
  {
    DWORD error = GetLastError();
    throw Azure::Core::Http::TransportException(
        "Unable to transcode the input wide string to a string. Error Code: "
        + std::to_string(error) + ".");
  }
  return str;
}

std::string WideStringToStringASCII(
    std::vector<WCHAR>::iterator wideStringStart,
    std::vector<WCHAR>::iterator wideStringEnd)
{
  // Converting this way is only safe when the text is ASCII.
#pragma warning(suppress : 4244)
  std::string str(wideStringStart, wideStringEnd);
  return str;
}

void ParseHttpVersion(
    const std::string& httpVersion,
    uint16_t* majorVersion,
    uint16_t* minorVersion)
{
  auto httpVersionEnd = httpVersion.end();

  // Set response code and HTTP version (i.e. HTTP/1.1)
  auto majorVersionStart
      = httpVersion.begin() + 5; // HTTP = 4, / = 1, moving to 5th place for version
  auto majorVersionEnd = std::find(majorVersionStart, httpVersionEnd, '.');
  auto majorVersionInt = std::stoi(std::string(majorVersionStart, majorVersionEnd));

  auto minorVersionStart = majorVersionEnd + 1; // start of minor version
  auto minorVersionInt = std::stoi(std::string(minorVersionStart, httpVersionEnd));

  *majorVersion = static_cast<uint16_t>(majorVersionInt);
  *minorVersion = static_cast<uint16_t>(minorVersionInt);
}

/**
 * @brief Add a list of HTTP headers to the #Azure::Core::Http::RawResponse.
 *
 * @remark The \p headers must contain valid header name characters (RFC 7230).
 * @remark Header name, value and delimiter are expected to be in \p headers.
 *
 * @param headers The complete list of headers to be added, in the form "name:value\0",
 * terminated by "\0".
 *
 * @throw if \p headers has an invalid header name or if the delimiter is missing.
 */
void SetHeaders(std::string const& headers, std::unique_ptr<RawResponse>& rawResponse)
{
  auto begin = headers.data();
  auto end = begin + headers.size();

  while (begin < end)
  {
    auto delimiter = std::find(begin, end, '\0');
    if (delimiter < end)
    {
      Azure::Core::Http::_detail::RawResponseHelpers::SetHeader(
          *rawResponse,
          reinterpret_cast<uint8_t const*>(begin),
          reinterpret_cast<uint8_t const*>(delimiter));
    }
    else
    {
      break;
    }
    begin = delimiter + 1;
  }
}

std::string GetHeadersAsString(Azure::Core::Http::Request const& request)
{
  std::string requestHeaderString;

  // request.GetHeaders() aggregates the pre- and post-retry headers into a single map. Capture it
  // so we don't recalculate the merge multiple times.
  auto requestHeaders = request.GetHeaders();

  for (auto const& header : requestHeaders)
  {
    requestHeaderString += header.first; // string (key)
    requestHeaderString += ": ";
    requestHeaderString += header.second; // string's value
    requestHeaderString += "\r\n";
  }

  // The test recording infrastructure requires that a Patch verb have a Content-Length header,
  // because it does not distinguish between requests with and without a body if there's no
  // Content-Length header.
  if (request.GetMethod() == HttpMethod::Patch)
  {
    if (requestHeaders.find("Content-Length") == requestHeaders.end())
    {
      if (request.GetBodyStream() == nullptr || request.GetBodyStream()->Length() == 0)
      {
        requestHeaderString += "Content-Length: 0\r\n";
      }
    }
  }

  requestHeaderString += "\r\n";

  return requestHeaderString;
}
} // namespace

void Azure::Core::_detail::FreeWinHttpHandleImpl(void* obj)
{
  // If definitions from windows.h are only being used as private members and not a public API, we
  // don't want to include windows.h in inc/ headers, so that it does not end up being included in
  // customer code.
  // Formally, WinHttpCloseHandle() takes HINTERNET, which is LPVOID, which is void*. That is why we
  // defined it that way in the header, and here, we are going to static_assert that it is the same
  // type and safely cast it to HINTERNET.
  static_assert(std::is_same<HINTERNET, void*>::value, "HINTERNET == void*");
  WinHttpCloseHandle(static_cast<HINTERNET>(obj));
}

// For each certificate specified in trustedCertificate, add to certificateStore.
bool _detail::WinHttpRequest::AddCertificatesToStore(
    std::vector<std::string> const& trustedCertificates,
    HCERTSTORE certificateStore) const
{
  for (auto const& trustedCertificate : trustedCertificates)
  {
    auto derCertificate = Azure::Core::Convert::Base64Decode(trustedCertificate);

    if (!CertAddEncodedCertificateToStore(
            certificateStore,
            X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
            derCertificate.data(),
            static_cast<DWORD>(derCertificate.size()),
            CERT_STORE_ADD_NEW,
            NULL))
    {
      GetErrorAndThrow("CertAddEncodedCertificateToStore failed");
    }
  }
  return true;
}

// VerifyCertificateInChain determines whether the certificate in serverCertificate
// chains up to one of the certificates represented by trustedCertificate or not.
bool _detail::WinHttpRequest::VerifyCertificatesInChain(
    std::vector<std::string> const& trustedCertificates,
    PCCERT_CONTEXT serverCertificate) const
{
  if ((trustedCertificates.empty()) || !serverCertificate)
  {
    return false;
  }

  // Creates an in-memory certificate store that is destroyed at end of this function.
  wil::unique_hcertstore certificateStore(CertOpenStore(
      CERT_STORE_PROV_MEMORY,
      X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
      0,
      CERT_STORE_CREATE_NEW_FLAG,
      nullptr));
  if (!certificateStore)
  {
    GetErrorAndThrow("CertOpenStore failed");
  }

  // Add the trusted certificates to that store.
  if (!AddCertificatesToStore(trustedCertificates, certificateStore.get()))
  {
    Log::Write(Logger::Level::Error, "Cannot add certificates to store");
    return false;
  }

  // WIL doesn't declare a convenient wrapper for a HCERTCHAINENGINE, so we define a custom one.
  wil::unique_any<
      HCERTCHAINENGINE,
      decltype(CertFreeCertificateChainEngine),
      CertFreeCertificateChainEngine>
      certificateChainEngine;
  {
    CERT_CHAIN_ENGINE_CONFIG EngineConfig{};
    EngineConfig.cbSize = sizeof(EngineConfig);
    EngineConfig.dwFlags = CERT_CHAIN_ENABLE_CACHE_AUTO_UPDATE | CERT_CHAIN_ENABLE_SHARE_STORE;
    EngineConfig.hExclusiveRoot = certificateStore.get();

    if (!CertCreateCertificateChainEngine(&EngineConfig, certificateChainEngine.addressof()))
    {
      GetErrorAndThrow("CertCreateCertificateChainEngine failed");
    }
  }

  // Generate a certificate chain using the local chain engine and the certificate store containing
  // the trusted certificates.
  wil::unique_cert_chain_context chainContextToVerify;
  {
    CERT_CHAIN_PARA ChainPara{};
    ChainPara.cbSize = sizeof(ChainPara);
    if (!CertGetCertificateChain(
            certificateChainEngine.get(),
            serverCertificate,
            nullptr,
            certificateStore.get(),
            &ChainPara,
            0,
            nullptr,
            chainContextToVerify.addressof()))
    {
      GetErrorAndThrow("CertGetCertificateChain failed");
    }
  }

  // And make sure that the certificate chain which was created matches the SSL chain.
  {
    CERT_CHAIN_POLICY_PARA PolicyPara{};
    PolicyPara.cbSize = sizeof(PolicyPara);

    CERT_CHAIN_POLICY_STATUS PolicyStatus{};
    PolicyStatus.cbSize = sizeof(PolicyStatus);

    if (!CertVerifyCertificateChainPolicy(
            CERT_CHAIN_POLICY_SSL, chainContextToVerify.get(), &PolicyPara, &PolicyStatus))
    {
      GetErrorAndThrow("CertVerifyCertificateChainPolicy");
    }
    if (PolicyStatus.dwError != 0)
    {
      Log::Write(
          Logger::Level::Error,
          "CertVerifyCertificateChainPolicy sets certificateStatus "
              + std::to_string(PolicyStatus.dwError));
      return false;
    }
  }
  return true;
}

namespace {

// If the `internetStatus` value has `id` bit set, then append the name of `id` to the string `rv`.
#define APPEND_ENUM_STRING(id) \
  if (internetStatus & (id)) \
  { \
    rv += #id " "; \
  }
std::string InternetStatusToString(DWORD internetStatus)
{
  std::string rv;
  APPEND_ENUM_STRING(WINHTTP_CALLBACK_STATUS_RESOLVING_NAME);
  APPEND_ENUM_STRING(WINHTTP_CALLBACK_STATUS_NAME_RESOLVED);
  APPEND_ENUM_STRING(WINHTTP_CALLBACK_STATUS_CONNECTING_TO_SERVER);
  APPEND_ENUM_STRING(WINHTTP_CALLBACK_STATUS_CONNECTED_TO_SERVER);
  APPEND_ENUM_STRING(WINHTTP_CALLBACK_STATUS_SENDING_REQUEST);
  APPEND_ENUM_STRING(WINHTTP_CALLBACK_STATUS_REQUEST_SENT);
  APPEND_ENUM_STRING(WINHTTP_CALLBACK_STATUS_RECEIVING_RESPONSE);
  APPEND_ENUM_STRING(WINHTTP_CALLBACK_STATUS_RESPONSE_RECEIVED);
  APPEND_ENUM_STRING(WINHTTP_CALLBACK_STATUS_CLOSING_CONNECTION);
  APPEND_ENUM_STRING(WINHTTP_CALLBACK_STATUS_CONNECTION_CLOSED);
  APPEND_ENUM_STRING(WINHTTP_CALLBACK_STATUS_HANDLE_CREATED);
  APPEND_ENUM_STRING(WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING);
  APPEND_ENUM_STRING(WINHTTP_CALLBACK_STATUS_DETECTING_PROXY);
  APPEND_ENUM_STRING(WINHTTP_CALLBACK_STATUS_REDIRECT);
  APPEND_ENUM_STRING(WINHTTP_CALLBACK_STATUS_INTERMEDIATE_RESPONSE);
  APPEND_ENUM_STRING(WINHTTP_CALLBACK_STATUS_SECURE_FAILURE);
  APPEND_ENUM_STRING(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE);
  APPEND_ENUM_STRING(WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE);
  APPEND_ENUM_STRING(WINHTTP_CALLBACK_STATUS_READ_COMPLETE);
  APPEND_ENUM_STRING(WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE);
  APPEND_ENUM_STRING(WINHTTP_CALLBACK_STATUS_REQUEST_ERROR);
  APPEND_ENUM_STRING(WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE);
  APPEND_ENUM_STRING(WINHTTP_CALLBACK_STATUS_GETPROXYFORURL_COMPLETE);
  APPEND_ENUM_STRING(WINHTTP_CALLBACK_STATUS_CLOSE_COMPLETE);
  APPEND_ENUM_STRING(WINHTTP_CALLBACK_STATUS_SHUTDOWN_COMPLETE);
  // For <reasons> this is not defined on the Win2022 Azure DevOps image, so manually expand it.
  //  APPEND_ENUM_STRING(WINHTTP_CALLBACK_STATUS_GETPROXYSETTINGS_COMPLETE);
  if (internetStatus & 0x08000000)
  {
    rv += std::string("WINHTTP_CALLBACK_STATUS_GETPROXYSETTINGS_COMPLETE") + " ";
  }
  APPEND_ENUM_STRING(WINHTTP_CALLBACK_STATUS_SETTINGS_WRITE_COMPLETE);
  APPEND_ENUM_STRING(WINHTTP_CALLBACK_STATUS_SETTINGS_READ_COMPLETE);
  return rv;
}
#undef APPEND_ENUM_STRING
} // namespace

namespace Azure { namespace Core { namespace Http { namespace _detail {

  bool WinHttpAction::RegisterWinHttpStatusCallback(
      Azure::Core::_internal::UniqueHandle<HINTERNET> const& internetHandle)
  {
    return (
        WinHttpSetStatusCallback(
            internetHandle.get(),
            &WinHttpAction::StatusCallback,
            WINHTTP_CALLBACK_FLAG_ALL_NOTIFICATIONS,
            0)
        != WINHTTP_INVALID_STATUS_CALLBACK);
  }

  /**
   * Wait for an action to complete.
   *
   */
  bool WinHttpAction::WaitForAction(
      std::function<void()> initiateAction,
      DWORD expectedCallbackStatus,
      Azure::Core::Context const& context,
      Azure::DateTime::duration const& pollDuration)
  {
    // Before doing any work, check to make sure that the context hasn't already been cancelled.
    context.ThrowIfCancelled();

    // By definition, there cannot be any actions outstanding at this point because we have not
    // yet called initiateAction. So it's safe to reset our state here.
    ResetEvent(m_actionCompleteEvent.get());
    m_expectedStatus = expectedCallbackStatus;
    m_stowedError = 0;
    m_stowedErrorInformation = 0;
    m_bytesAvailable = 0;

    // Call the provided callback to start the WinHTTP action.
    initiateAction();

    DWORD waitResult;
    do
    {
      waitResult = WaitForSingleObject(
          m_actionCompleteEvent.get(),
          static_cast<DWORD>(
              std::chrono::duration_cast<std::chrono::milliseconds>(pollDuration).count()));
      if (waitResult == WAIT_TIMEOUT)
      {
        // If the request was cancelled while we were waiting, throw an exception.
        if (context.IsCancelled())
        {
          Log::Stream(Logger::Level::Error)
              << "Request was cancelled while waiting for action to complete." << std::endl;
        }
        context.ThrowIfCancelled();
      }
      else if (waitResult != WAIT_OBJECT_0)
      {
        Log::Stream(Logger::Level::Error)
            << "WaitForSingleObject failed with error code " << GetLastError() << std::endl;
        return false;
      }
    } while (waitResult != WAIT_OBJECT_0);
    if (m_stowedError != NO_ERROR)
    {
      Log::Stream(Logger::Level::Error)
          << "Action completed with error: " << GetErrorMessage(m_stowedError);
      return false;
    }
    return true;
  }

  void WinHttpAction::CompleteAction()
  {
    auto scope_exit{m_actionCompleteEvent.SetEvent_scope_exit()};
  }
  void WinHttpAction::CompleteActionWithData(DWORD bytesAvailable)
  {
    // Note that the order of scope_exit and lock is important - this ensures that scope_exit is
    // destroyed *after* lock is destroyed, ensuring that the event is not set to the signalled
    // state before the lock is released.
    auto scope_exit{m_actionCompleteEvent.SetEvent_scope_exit()};
    std::unique_lock<std::mutex> lock(m_actionCompleteMutex);
    m_bytesAvailable = bytesAvailable;
  }
  void WinHttpAction::CompleteActionWithError(DWORD_PTR stowedErrorInformation, DWORD stowedError)
  {
    if (m_expectedStatus != WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING)
    {
      // Note that the order of scope_exit and lock is important - this ensures that scope_exit is
      // destroyed *after* lock is destroyed, ensuring that the event is not set to the signalled
      // state before the lock is released.
      auto scope_exit{m_actionCompleteEvent.SetEvent_scope_exit()};
      std::unique_lock<std::mutex> lock(m_actionCompleteMutex);
      m_stowedErrorInformation = stowedErrorInformation;
      m_stowedError = stowedError;
    }
    else
    {
      Log::Write(
          Logger::Level::Verbose, "Received error while closing: " + std::to_string(stowedError));
    }
  }

  DWORD WinHttpAction::GetStowedError()
  {
    std::unique_lock<std::mutex> lock(m_actionCompleteMutex);
    return m_stowedError;
  }
  DWORD_PTR WinHttpAction::GetStowedErrorInformation()
  {
    std::unique_lock<std::mutex> lock(m_actionCompleteMutex);
    return m_stowedErrorInformation;
  }
  DWORD WinHttpAction::GetBytesAvailable()
  {
    std::unique_lock<std::mutex> lock(m_actionCompleteMutex);
    return m_bytesAvailable;
  }

  /**
   * Called by WinHTTP when sending a request to the server. This callback allows us to inspect
   * the TLS certificate before sending it to the server.
   */
  void WinHttpAction::StatusCallback(
      HINTERNET hInternet,
      DWORD_PTR dwContext,
      DWORD internetStatus,
      LPVOID statusInformation,
      DWORD statusInformationLength)
  {
    // If we're called before our context has been set (on Open and Close callbacks), ignore the
    // status callback.
    if (dwContext == 0)
    {
      return;
    }
    WinHttpAction* httpAction = reinterpret_cast<WinHttpAction*>(dwContext);
    try
    {
      httpAction->OnHttpStatusOperation(
          hInternet, internetStatus, statusInformation, statusInformationLength);
    }
    catch (Azure::Core::RequestFailedException const& rfe)
    {
      // If an exception is thrown in the handler, log the error and terminate the connection.
      Log::Write(
          Logger::Level::Error,
          "Request Failed Exception Thrown: " + std::string(rfe.what()) + rfe.Message);
      WinHttpCloseHandle(hInternet);
      httpAction->m_httpRequest->MarkRequestHandleClosed();
    }
    catch (std::exception const& ex)
    {
      // If an exception is thrown in the handler, log the error and terminate the connection.
      Log::Write(Logger::Level::Error, "Exception Thrown: " + std::string(ex.what()));
    }
  }
  namespace {
    std::string WinHttpAsyncResultToString(DWORD_PTR result)
    {
      switch (result)
      {
        case API_RECEIVE_RESPONSE:
          return "API_RECEIVE_RESPONSE";
        case API_QUERY_DATA_AVAILABLE:
          return "API_QUERY_DATA_AVAILABLE";
        case API_READ_DATA:
          return "API_READ_DATA";
        case API_WRITE_DATA:
          return "API_WRITE_DATA";
        case API_SEND_REQUEST:
          return "API_SEND_REQUEST";
        case API_GET_PROXY_FOR_URL:
          return "API_GET_PROXY_FOR_URL";
        default:
          return "Unknown (" + std::to_string(result) + ")";
      }
    }
  } // namespace
  /**
   * @brief HTTP Callback to enable private certificate checks.
   *
   * This method is called by WinHTTP when a certificate is received. This method is called
   * multiple times based on the state of the TLS connection.
   *
   * Special consideration for the WINHTTP_CALLBACK_STATUS_SENDING_REQUEST - this callback is
   * called during the TLS connection - if a TLS root certificate is configured, we verify that
   * the certificate chain sent from the server contains the certificate the HTTP client was
   * configured with. If it is, we accept the connection, if it is not, we abort the connection,
   * closing the incoming request handle.
   */
  void WinHttpAction::OnHttpStatusOperation(
      HINTERNET hInternet,
      DWORD internetStatus,
      LPVOID statusInformation,
      DWORD statusInformationLength)
  {
    Log::Write(
        Logger::Level::Informational,
        "Status operation: " + std::to_string(internetStatus) + "("
            + InternetStatusToString(internetStatus) + ")");
    if (internetStatus == WINHTTP_CALLBACK_STATUS_SECURE_FAILURE)
    {
      Log::Write(Logger::Level::Error, "Security failure. :(");
    }
    else if (internetStatus == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR)
    {
      WINHTTP_ASYNC_RESULT* asyncResult = static_cast<WINHTTP_ASYNC_RESULT*>(statusInformation);
      Log::Write(
          Logger::Level::Error,
          "Request error: " + GetErrorMessage(asyncResult->dwError)
              + " Failing API: " + WinHttpAsyncResultToString(asyncResult->dwResult));
      CompleteActionWithError(asyncResult->dwResult, asyncResult->dwError);
    }
    else if (internetStatus == WINHTTP_CALLBACK_STATUS_SENDING_REQUEST)
    {
      // We will only set the Status callback if a root certificate has been set. There is no
      // action which needs to be completed for this notification.
      m_httpRequest->HandleExpectedTlsRootCertificates(hInternet);
    }
    else if (internetStatus == m_expectedStatus)
    {
      switch (internetStatus)
      {
        case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
          // A WinHttpSendRequest API call has completed, complete the current action.
          CompleteAction();
          break;
        case WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE:
          // A WinHttpWriteData call has completed, complete the current action.
          CompleteAction();
          break;
        case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
          // Headers for an HTTP response are available, complete the current action.
          CompleteAction();
          break;
        case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
          // A WinHttpReadData call has completed. Complete the current action, including the
          // amount of data read.
          CompleteActionWithData(statusInformationLength);
          break;
        case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
          // An HINTERNET handle is closing, complete the outstanding close request.
          Log::Write(
              Logger::Level::Verbose, "Closing handle; completing outstanding Close request");
          CompleteAction();
          break;
        default:
          Log::Write(
              Logger::Level::Error,
              "Received expected status " + InternetStatusToString(internetStatus)
                  + " but it was not handled.");
          break;
      }
    }
  }

  void WinHttpRequest::HandleExpectedTlsRootCertificates(HINTERNET hInternet)
  {
    if (!m_expectedTlsRootCertificates.empty())
    {
      // Ask WinHTTP for the server certificate - this won't be valid outside a status callback.
      wil::unique_cert_context serverCertificate;
      {
        DWORD bufferLength = sizeof(PCCERT_CONTEXT);
        if (!WinHttpQueryOption(
                hInternet,
                WINHTTP_OPTION_SERVER_CERT_CONTEXT,
                reinterpret_cast<void*>(serverCertificate.addressof()),
                &bufferLength))
        {
          GetErrorAndThrow("Could not retrieve TLS server certificate.");
        }
      }

      if (!VerifyCertificatesInChain(m_expectedTlsRootCertificates, serverCertificate.get()))
      {
        Log::Write(
            Logger::Level::Error, "Server certificate is not trusted.  Aborting HTTP request");

        // To signal to caller that the request is to be terminated, the callback closes the
        // handle. This ensures that no message is sent to the server.
        WinHttpCloseHandle(hInternet);

        // To avoid a double free of this handle record that we've
        // already closed the handle.
        m_requestHandleClosed = true;

        // And we're done processing the request, return because there's nothing
        // else to do.
        return;
      }
    }
  }
}}}} // namespace Azure::Core::Http::_detail

Azure::Core::_internal::UniqueHandle<HINTERNET> WinHttpTransport::CreateSessionHandle()
{
  // Use WinHttpOpen to obtain a session handle.
  // The dwFlags is set to 0 - all WinHTTP functions are performed synchronously.
  Azure::Core::_internal::UniqueHandle<HINTERNET> sessionHandle(WinHttpOpen(
      NULL, // Do not use a fallback user-agent string, and only rely on the header within the
            // request itself.
      // If the customer asks for it, enable use of the system default HTTP proxy.
      (m_options.EnableSystemDefaultProxy ? WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
                                          : WINHTTP_ACCESS_TYPE_NO_PROXY),
      WINHTTP_NO_PROXY_NAME,
      WINHTTP_NO_PROXY_BYPASS,
      WINHTTP_FLAG_ASYNC)); // All requests on this session are performed asynchronously.

  if (!sessionHandle)
  {
    // Errors include:
    // ERROR_WINHTTP_INTERNAL_ERROR
    // ERROR_NOT_ENOUGH_MEMORY
    GetErrorAndThrow("Error while getting a session handle.");
  }

  // These options are only available starting from Windows 10 Version 2004, starting 06/09/2020.
  // These are primarily round trip time (RTT) performance optimizations, and hence if they don't
  // get set successfully, we shouldn't fail the request and continue as if the options don't exist.
  // Therefore, we just ignore the error and move on.

  // TCP_FAST_OPEN has a bug when the DNS resolution fails which can result
  // in a leak.  Until that issue is fixed we've disable this option.

#if defined(WINHTTP_OPTION_TCP_FAST_OPEN) && FALSE
  BOOL tcp_fast_open = TRUE;
  WinHttpSetOption(
      sessionHandle.get(), WINHTTP_OPTION_TCP_FAST_OPEN, &tcp_fast_open, sizeof(tcp_fast_open));
#endif

#ifdef WINHTTP_OPTION_TLS_FALSE_START
  BOOL tls_false_start = TRUE;
  WinHttpSetOption(
      sessionHandle.get(),
      WINHTTP_OPTION_TLS_FALSE_START,
      &tls_false_start,
      sizeof(tls_false_start));
#endif

  // Enforce TLS version 1.2 or 1.3 (if available).
  auto tlsOption = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#if defined(WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3)
  tlsOption |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
  if (!WinHttpSetOption(
          sessionHandle.get(), WINHTTP_OPTION_SECURE_PROTOCOLS, &tlsOption, sizeof(tlsOption)))
  {
#if defined(WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3)
    // If TLS 1.3 is not available, try to set TLS 1.2 only.
    tlsOption = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    if (!WinHttpSetOption(
            sessionHandle.get(), WINHTTP_OPTION_SECURE_PROTOCOLS, &tlsOption, sizeof(tlsOption)))
    {
#endif
      GetErrorAndThrow("Error while enforcing TLS version for connection request.");
#if defined(WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3)
    }
#endif
  }

  return sessionHandle;
}

namespace {
WinHttpTransportOptions WinHttpTransportOptionsFromTransportOptions(
    Azure::Core::Http::Policies::TransportOptions const& transportOptions)
{
  WinHttpTransportOptions httpOptions;
  if (transportOptions.HttpProxy.HasValue())
  {
    // WinHTTP proxy strings are semicolon separated elements, each of which
    // has the following format:
    //  ([<scheme>=][<scheme>"://"]<server>[":"<port>])
    std::string proxyString;
    proxyString = "http=" + transportOptions.HttpProxy.Value();
    proxyString += ";";
    proxyString += "https=" + transportOptions.HttpProxy.Value();
    httpOptions.ProxyInformation = proxyString;
  }
  httpOptions.ProxyUserName = transportOptions.ProxyUserName;
  httpOptions.ProxyPassword = transportOptions.ProxyPassword;
  // Note that WinHTTP accepts a set of root certificates, even though transportOptions only
  // specifies a single one.
  if (!transportOptions.ExpectedTlsRootCertificate.empty())
  {
    httpOptions.ExpectedTlsRootCertificates.push_back(transportOptions.ExpectedTlsRootCertificate);
  }
  if (transportOptions.EnableCertificateRevocationListCheck)
  {
    httpOptions.EnableCertificateRevocationListCheck = true;
  }
  // If you specify an expected TLS root certificate, you also need to enable ignoring unknown
  // CAs.
  if (!transportOptions.ExpectedTlsRootCertificate.empty())
  {
    httpOptions.IgnoreUnknownCertificateAuthority = true;
  }

  if (transportOptions.DisableTlsCertificateValidation)
  {
    httpOptions.IgnoreUnknownCertificateAuthority = true;
    httpOptions.IgnoreInvalidCertificateCommonName = true;
  }

  return httpOptions;
}
} // namespace

WinHttpTransport::WinHttpTransport(WinHttpTransportOptions const& options)
    : m_options(options), m_sessionHandle(CreateSessionHandle())
{
}

WinHttpTransport::WinHttpTransport(
    Azure::Core::Http::Policies::TransportOptions const& transportOptions)
    : WinHttpTransport(WinHttpTransportOptionsFromTransportOptions(transportOptions))
{
}

WinHttpTransport::~WinHttpTransport() = default;

Azure::Core::_internal::UniqueHandle<HINTERNET> WinHttpTransport::CreateConnectionHandle(
    Azure::Core::Url const& url,
    Azure::Core::Context const& context)
{
  // If port is 0, i.e. INTERNET_DEFAULT_PORT, it uses port 80 for HTTP and port 443 for HTTPS.
  uint16_t port = url.GetPort();

  // Before doing any work, check to make sure that the context hasn't already been cancelled.
  context.ThrowIfCancelled();

  // Specify an HTTP server.
  // This function always operates synchronously.
  Azure::Core::_internal::UniqueHandle<HINTERNET> rv(WinHttpConnect(
      m_sessionHandle.get(),
      StringToWideString(url.GetHost()).c_str(),
      port == 0 ? INTERNET_DEFAULT_PORT : port,
      0));

  if (!rv)
  {
    // Errors include:
    // ERROR_WINHTTP_INCORRECT_HANDLE_TYPE
    // ERROR_WINHTTP_INTERNAL_ERROR
    // ERROR_WINHTTP_INVALID_URL
    // ERROR_WINHTTP_OPERATION_CANCELLED
    // ERROR_WINHTTP_UNRECOGNIZED_SCHEME
    // ERROR_WINHTTP_SHUTDOWN
    // ERROR_NOT_ENOUGH_MEMORY
    GetErrorAndThrow("Error while getting a connection handle.");
  }
  return rv;
}

void _detail::WinHttpRequest::EnableWebSocketsSupport()
{
#pragma warning(push)
  // warning C6387: _Param_(3) could be '0'.
#pragma warning(disable : 6387)
  if (!WinHttpSetOption(m_requestHandle.get(), WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0))
#pragma warning(pop)
  {
    GetErrorAndThrow("Error while Enabling WebSocket upgrade.");
  }
}

_detail::WinHttpRequest::WinHttpRequest(
    Azure::Core::_internal::UniqueHandle<HINTERNET> const& connectionHandle,
    Azure::Core::Url const& url,
    Azure::Core::Http::HttpMethod const& method,
    WinHttpTransportOptions const& options)
    : m_expectedTlsRootCertificates(options.ExpectedTlsRootCertificates)
{
  const std::string& path = url.GetRelativeUrl();
  HttpMethod requestMethod = method;
  bool const requestSecureHttp(
      !Azure::Core::_internal::StringExtensions::LocaleInvariantCaseInsensitiveEqual(
          url.GetScheme(), HttpScheme)
      && !Azure::Core::_internal::StringExtensions::LocaleInvariantCaseInsensitiveEqual(
          url.GetScheme(), WebSocketScheme));

  // Create an HTTP request handle.
  m_requestHandle.reset(WinHttpOpenRequest(
      connectionHandle.get(),
      HttpMethodToWideString(requestMethod).c_str(),
      path.empty() ? NULL : StringToWideString(path).c_str(), // Name of the target resource of
                                                              // the specified HTTP verb
      NULL, // Use HTTP/1.1
      WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES, // No media types are accepted by the client
      requestSecureHttp ? WINHTTP_FLAG_SECURE : 0)); // Uses secure transaction semantics (SSL/TLS)
  if (!m_requestHandle)
  {
    // Errors include:
    // ERROR_WINHTTP_INCORRECT_HANDLE_TYPE
    // ERROR_WINHTTP_INTERNAL_ERROR
    // ERROR_WINHTTP_INVALID_URL
    // ERROR_WINHTTP_OPERATION_CANCELLED
    // ERROR_WINHTTP_UNRECOGNIZED_SCHEME
    // ERROR_NOT_ENOUGH_MEMORY
    GetErrorAndThrow("Error while getting a request handle.");
  }

  if (requestSecureHttp)
  {
    // If the service requests TLS client certificates, we want to let the WinHTTP APIs know that
    // it's ok to initiate the request without a client certificate.
    //
    // Note: If/When TLS client certificate support is added to the pipeline, this line may need to
    // be revisited.
    if (!WinHttpSetOption(
            m_requestHandle.get(),
            WINHTTP_OPTION_CLIENT_CERT_CONTEXT,
            WINHTTP_NO_CLIENT_CERT_CONTEXT,
            0))
    {
      GetErrorAndThrow("Error while setting client cert context to ignore.");
    }
  }

  if (!options.ProxyInformation.empty())
  {
    WINHTTP_PROXY_INFO proxyInfo{};
    std::wstring proxyWide{StringToWideString(options.ProxyInformation)};
    proxyInfo.dwAccessType = WINHTTP_ACCESS_TYPE_NAMED_PROXY;
    proxyInfo.lpszProxy = const_cast<LPWSTR>(proxyWide.c_str());
    proxyInfo.lpszProxyBypass = WINHTTP_NO_PROXY_BYPASS;
    if (!WinHttpSetOption(
            m_requestHandle.get(), WINHTTP_OPTION_PROXY, &proxyInfo, sizeof(proxyInfo)))
    {
      GetErrorAndThrow("Error while setting Proxy information.");
    }
  }
  if (options.ProxyUserName.HasValue() || options.ProxyPassword.HasValue())
  {
    if (!WinHttpSetCredentials(
            m_requestHandle.get(),
            WINHTTP_AUTH_TARGET_PROXY,
            WINHTTP_AUTH_SCHEME_BASIC,
            StringToWideString(options.ProxyUserName.Value()).c_str(),
            StringToWideString(options.ProxyPassword.Value()).c_str(),
            0))
    {
      GetErrorAndThrow("Error while setting Proxy credentials.");
    }
  }

  if (options.IgnoreUnknownCertificateAuthority || !options.ExpectedTlsRootCertificates.empty())
  {
    auto option = SECURITY_FLAG_IGNORE_UNKNOWN_CA;
    if (!WinHttpSetOption(
            m_requestHandle.get(), WINHTTP_OPTION_SECURITY_FLAGS, &option, sizeof(option)))
    {
      GetErrorAndThrow("Error while setting ignore unknown server certificate.");
    }
  }

  if (options.IgnoreInvalidCertificateCommonName)
  {
    auto option = SECURITY_FLAG_IGNORE_CERT_CN_INVALID;
    if (!WinHttpSetOption(
            m_requestHandle.get(), WINHTTP_OPTION_SECURITY_FLAGS, &option, sizeof(option)))
    {
      GetErrorAndThrow("Error while setting ignore invalid certificate common name.");
    }
  }

  if (options.EnableCertificateRevocationListCheck)
  {
    DWORD value = WINHTTP_ENABLE_SSL_REVOCATION;
    if (!WinHttpSetOption(
            m_requestHandle.get(), WINHTTP_OPTION_ENABLE_FEATURE, &value, sizeof(value)))
    {
      GetErrorAndThrow("Error while enabling CRL validation.");
    }
  }

  // Set the callback function to be called whenever the state of the request handle changes.
  m_httpAction = std::make_unique<_detail::WinHttpAction>(this);

  if (!m_httpAction->RegisterWinHttpStatusCallback(m_requestHandle))
  {
    GetErrorAndThrow("Error while setting up the status callback.");
  }
}

/*
 * Destructor for WinHTTP request. Closes the request handle.
 */
_detail::WinHttpRequest::~WinHttpRequest()
{
  if (!m_requestHandleClosed)
  {
    Log::Write(
        Logger::Level::Informational,
        "WinHttpRequest::~WinHttpRequest. Closing handle synchronously.");

    // Close the outstanding request handle, waiting until the HANDLE_CLOSING status is received.
    if (!m_httpAction->WaitForAction(
            [this]() {
              auto requestHandle = m_requestHandle.release();
              if (!WinHttpCloseHandle(requestHandle))
              {
                Log::Write(
                    Logger::Level::Error,
                    "Error closing WinHTTP handle: " + GetErrorMessage(GetLastError()));
              }
            },

            WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING,
            Azure::Core::Context{}))
    {
      Log::Write(Logger::Level::Error, "Error while closing the request handle.");
    }
    Log::Write(Logger::Level::Informational, "WinHttpRequest::~WinHttpRequest. Handle closed.");
  }
}

std::unique_ptr<_detail::WinHttpRequest> WinHttpTransport::CreateRequestHandle(
    Azure::Core::_internal::UniqueHandle<HINTERNET> const& connectionHandle,
    Azure::Core::Url const& url,
    Azure::Core::Http::HttpMethod const& method)
{
  auto request{std::make_unique<_detail::WinHttpRequest>(connectionHandle, url, method, m_options)};
  // If we are supporting WebSockets, then let WinHTTP know that it should
  // prepare to upgrade the HttpRequest to a WebSocket.
  if (HasWebSocketSupport())
  {
    request->EnableWebSocketsSupport();
  }
  return request;
}

// For PUT/POST requests, send additional data using WinHttpWriteData.
void _detail::WinHttpRequest::Upload(
    Azure::Core::Http::Request& request,
    Azure::Core::Context const& context)
{
  auto streamBody = request.GetBodyStream();
  int64_t streamLength = streamBody->Length();

  // Consider using `MaximumUploadChunkSize` here, after some perf measurements
  size_t uploadChunkSize = DefaultUploadChunkSize;
  if (streamLength < MaximumUploadChunkSize)
  {
    uploadChunkSize = static_cast<size_t>(streamLength);
  }
  auto unique_buffer = std::make_unique<uint8_t[]>(uploadChunkSize);

  while (true)
  {
    size_t rawRequestLen = streamBody->Read(unique_buffer.get(), uploadChunkSize, context);
    if (rawRequestLen == 0)
    {
      break;
    }

    DWORD dwBytesWritten = 0;

    if (!m_httpAction->WaitForAction(
            [&]() { // Write data to the server.
              if (!WinHttpWriteData(
                      m_requestHandle.get(),
                      unique_buffer.get(),
                      static_cast<DWORD>(rawRequestLen),
                      &dwBytesWritten))
              {
                GetErrorAndThrow("Error while uploading/sending data.");
              }
            },
            WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE,
            context))

    {
      GetErrorAndThrow("Error sending HTTP request asynchronously", m_httpAction->GetStowedError());
    }
  }
}

void _detail::WinHttpRequest::SendRequest(
    Azure::Core::Http::Request& request,
    Azure::Core::Context const& context)
{
  std::wstring encodedHeaders;
  int encodedHeadersLength = 0;

  auto requestHeaders = request.GetHeaders();
  if (requestHeaders.size() != 0)
  {
    // The encodedHeaders will be null-terminated and the length is calculated.
    encodedHeadersLength = -1;
    std::string requestHeaderString = GetHeadersAsString(request);
    requestHeaderString.append("\0");

    encodedHeaders = StringToWideString(requestHeaderString);
  }

  int64_t streamLength = request.GetBodyStream()->Length();

  try
  {
    if (!m_httpAction->WaitForAction(
            [&]() {
              {
                // Send a request.
                // NB: DO NOT CHANGE THE TYPE OF THE CONTEXT PARAMETER WITHOUT UPDATING THE
                // HttpAction::StatusCallback method.
                if (!WinHttpSendRequest(
                        m_requestHandle.get(),
                        requestHeaders.size() == 0 ? WINHTTP_NO_ADDITIONAL_HEADERS
                                                   : encodedHeaders.c_str(),
                        encodedHeadersLength,
                        WINHTTP_NO_REQUEST_DATA,
                        0,
                        streamLength > 0 ? static_cast<DWORD>(streamLength) : 0,
                        reinterpret_cast<DWORD_PTR>(
                            m_httpAction
                                .get()))) // Context for WinHTTP status callbacks for this request.
                {
                  // Errors include:
                  // ERROR_WINHTTP_CANNOT_CONNECT
                  // ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED
                  // ERROR_WINHTTP_CONNECTION_ERROR
                  // ERROR_WINHTTP_INCORRECT_HANDLE_STATE
                  // ERROR_WINHTTP_INCORRECT_HANDLE_TYPE
                  // ERROR_WINHTTP_INTERNAL_ERROR
                  // ERROR_WINHTTP_INVALID_URL
                  // ERROR_WINHTTP_LOGIN_FAILURE
                  // ERROR_WINHTTP_NAME_NOT_RESOLVED
                  // ERROR_WINHTTP_OPERATION_CANCELLED
                  // ERROR_WINHTTP_RESPONSE_DRAIN_OVERFLOW
                  // ERROR_WINHTTP_SECURE_FAILURE
                  // ERROR_WINHTTP_SHUTDOWN
                  // ERROR_WINHTTP_TIMEOUT
                  // ERROR_WINHTTP_UNRECOGNIZED_SCHEME
                  // ERROR_NOT_ENOUGH_MEMORY
                  // ERROR_INVALID_PARAMETER
                  // ERROR_WINHTTP_RESEND_REQUEST
                  GetErrorAndThrow("Error while sending a request.");
                }
              }
            },
            WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE,
            context))
    {
      GetErrorAndThrow(
          "Error while waiting for a send to complete.", m_httpAction->GetStowedError());
    }

    // Chunked transfer encoding is not supported and the content length needs to be known up
    // front.
    if (streamLength == -1)
    {
      throw Azure::Core::Http::TransportException(
          "When uploading data, the body stream must have a known length.");
    }

    if (streamLength > 0)
    {
      Upload(request, context);
    }
  }
  catch (TransportException const&)
  {
    // If there was a TLS validation error, then we will have closed the request handle
    // during the TLS validation callback. So if an exception was thrown, if we force closed the
    // request handle, clear the handle in the requestHandle to prevent a double free.
    if (m_requestHandleClosed)
    {
      m_requestHandle.release();
    }
    throw;
  }
}

void _detail::WinHttpRequest::ReceiveResponse(Azure::Core::Context const& context)
{
  // Wait to receive the response to the HTTP request initiated by WinHttpSendRequest.
  // When WinHttpReceiveResponse completes successfully, the status code and response headers have
  // been received.
  if (!m_httpAction->WaitForAction(
          [this]() {
            if (!WinHttpReceiveResponse(m_requestHandle.get(), NULL))
            {
              // Errors include:
              // ERROR_WINHTTP_CANNOT_CONNECT
              // ERROR_WINHTTP_CHUNKED_ENCODING_HEADER_SIZE_OVERFLOW
              // ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED
              // ...
              // ERROR_WINHTTP_TIMEOUT
              // ERROR_WINHTTP_UNRECOGNIZED_SCHEME
              // ERROR_NOT_ENOUGH_MEMORY
              GetErrorAndThrow("Error while receiving a response.");
            }
          },
          WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE,
          context))
  {
    GetErrorAndThrow("Error while receiving a response.", m_httpAction->GetStowedError());
  }
}

int64_t _detail::WinHttpRequest::GetContentLength(
    HttpMethod requestMethod,
    HttpStatusCode responseStatusCode)
{
  DWORD dwContentLength = 0;
  DWORD dwSize = sizeof(dwContentLength);

  // For Head request, set the length of body response to 0.
  // Response will give us content-length as if we were not doing Head saying what would be the
  // length of the body. However, server won't send any body.
  // For NoContent status code, also need to set contentLength to 0.
  int64_t contentLength = 0;

  // Get the content length as a number.
  if (requestMethod != HttpMethod::Head && responseStatusCode != HttpStatusCode::NoContent)
  {
    if (!WinHttpQueryHeaders(
            m_requestHandle.get(),
            WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &dwContentLength,
            &dwSize,
            WINHTTP_NO_HEADER_INDEX))
    {
      contentLength = -1;
    }
    else
    {
      contentLength = static_cast<int64_t>(dwContentLength);
    }
  }

  return contentLength;
}

std::unique_ptr<RawResponse> _detail::WinHttpRequest::SendRequestAndGetResponse(
    HttpMethod requestMethod)
{
  // First, use WinHttpQueryHeaders to obtain the size of the buffer.
  // The call is expected to fail since no destination buffer is provided.
  DWORD sizeOfHeaders = 0;
  if (WinHttpQueryHeaders(
          m_requestHandle.get(),
          WINHTTP_QUERY_RAW_HEADERS,
          WINHTTP_HEADER_NAME_BY_INDEX,
          NULL,
          &sizeOfHeaders,
          WINHTTP_NO_HEADER_INDEX))
  {
    // WinHttpQueryHeaders was expected to fail.
    throw Azure::Core::Http::TransportException("Error while querying response headers.");
  }

  {
    DWORD error = GetLastError();
    if (error != ERROR_INSUFFICIENT_BUFFER)
    {
      GetErrorAndThrow("Error while querying response headers.", error);
    }
  }

  // Allocate memory for the buffer.
  std::vector<WCHAR> outputBuffer(sizeOfHeaders / sizeof(WCHAR), 0);

  // Now, use WinHttpQueryHeaders to retrieve all the headers.
  // Each header is terminated by "\0". An additional "\0" terminates the list of headers.
  if (!WinHttpQueryHeaders(
          m_requestHandle.get(),
          WINHTTP_QUERY_RAW_HEADERS,
          WINHTTP_HEADER_NAME_BY_INDEX,
          outputBuffer.data(),
          &sizeOfHeaders,
          WINHTTP_NO_HEADER_INDEX))
  {
    GetErrorAndThrow("Error while querying response headers.");
  }

  auto start = outputBuffer.begin();
  auto last = start + sizeOfHeaders / sizeof(WCHAR);
  auto statusLineEnd = std::find(start, last, '\0');
  start = statusLineEnd + 1; // start of headers
  std::string responseHeaders = WideStringToString(std::wstring(start, last));

  DWORD sizeOfHttp = sizeOfHeaders;

  // Get the HTTP version.
  if (!WinHttpQueryHeaders(
          m_requestHandle.get(),
          WINHTTP_QUERY_VERSION,
          WINHTTP_HEADER_NAME_BY_INDEX,
          outputBuffer.data(),
          &sizeOfHttp,
          WINHTTP_NO_HEADER_INDEX))
  {
    GetErrorAndThrow("Error while querying response headers.");
  }

  start = outputBuffer.begin();
  // Assuming ASCII here is OK since the input is expected to be an HTTP version string.
  std::string httpVersion = WideStringToStringASCII(start, start + sizeOfHttp / sizeof(WCHAR));

  uint16_t majorVersion = 0;
  uint16_t minorVersion = 0;
  ParseHttpVersion(httpVersion, &majorVersion, &minorVersion);

  DWORD statusCode = 0;
  DWORD dwSize = sizeof(statusCode);

  // Get the status code as a number.
  if (!WinHttpQueryHeaders(
          m_requestHandle.get(),
          WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
          WINHTTP_HEADER_NAME_BY_INDEX,
          &statusCode,
          &dwSize,
          WINHTTP_NO_HEADER_INDEX))
  {
    GetErrorAndThrow("Error while querying response headers.");
  }

  HttpStatusCode httpStatusCode = static_cast<HttpStatusCode>(statusCode);

  // Get the optional reason phrase.
  std::string reasonPhrase;
  DWORD sizeOfReasonPhrase = sizeOfHeaders;

  // HTTP/2 does not support reason phrase, refer to
  // https://www.rfc-editor.org/rfc/rfc7540#section-8.1.2.4.
  if (majorVersion == 1)
  {
    if (WinHttpQueryHeaders(
            m_requestHandle.get(),
            WINHTTP_QUERY_STATUS_TEXT,
            WINHTTP_HEADER_NAME_BY_INDEX,
            outputBuffer.data(),
            &sizeOfReasonPhrase,
            WINHTTP_NO_HEADER_INDEX))
    {
      // even with HTTP/1.1, we cannot assume that reason phrase is set since it is optional
      // according to https://www.rfc-editor.org/rfc/rfc2616.html#section-6.1.1.
      if (sizeOfReasonPhrase > 0)
      {
        start = outputBuffer.begin();
        reasonPhrase
            = WideStringToString(std::wstring(start, start + sizeOfReasonPhrase / sizeof(WCHAR)));
      }
    }
  }

  // Allocate the instance of the response on the heap with a shared ptr so this memory gets
  // delegated outside the transport and will be eventually released.
  auto rawResponse
      = std::make_unique<RawResponse>(majorVersion, minorVersion, httpStatusCode, reasonPhrase);

  SetHeaders(responseHeaders, rawResponse);

  return rawResponse;
}

std::unique_ptr<RawResponse> WinHttpTransport::Send(Request& request, Context const& context)
{
  Azure::Core::_internal::UniqueHandle<HINTERNET> connectionHandle
      = CreateConnectionHandle(request.GetUrl(), context);
  std::unique_ptr<_detail::WinHttpRequest> requestHandle(
      CreateRequestHandle(connectionHandle, request.GetUrl(), request.GetMethod()));

  requestHandle->SendRequest(request, context);
  requestHandle->ReceiveResponse(context);

  auto rawResponse{requestHandle->SendRequestAndGetResponse(request.GetMethod())};
  if (rawResponse && HasWebSocketSupport()
      && (rawResponse->GetStatusCode() == HttpStatusCode::SwitchingProtocols))
  {
    OnUpgradedConnection(requestHandle);
  }
  else
  {
    int64_t contentLength
        = requestHandle->GetContentLength(request.GetMethod(), rawResponse->GetStatusCode());

    rawResponse->SetBodyStream(
        std::make_unique<_detail::WinHttpStream>(requestHandle, contentLength));
  }
  return rawResponse;
}

size_t _detail::WinHttpRequest::ReadData(
    uint8_t* buffer,
    size_t count,
    Azure::Core::Context const& context)
{
  DWORD numberOfBytesRead = 0;
  if (!m_httpAction->WaitForAction(
          [&]() {
            if (!WinHttpReadData(
                    this->m_requestHandle.get(),
                    (LPVOID)(buffer),
                    static_cast<DWORD>(count),
                    &numberOfBytesRead))
            {
              // Errors include:
              // ERROR_WINHTTP_CONNECTION_ERROR
              // ERROR_WINHTTP_INCORRECT_HANDLE_STATE
              // ERROR_WINHTTP_INCORRECT_HANDLE_TYPE
              // ERROR_WINHTTP_INTERNAL_ERROR
              // ERROR_WINHTTP_OPERATION_CANCELLED
              // ERROR_WINHTTP_RESPONSE_DRAIN_OVERFLOW
              // ERROR_WINHTTP_TIMEOUT
              // ERROR_NOT_ENOUGH_MEMORY

              DWORD error = GetLastError();
              throw Azure::Core::Http::TransportException(
                  "Error while reading available data from the wire. Error Code: "
                  + std::to_string(error) + ".");
            }
            Log::Write(
                Logger::Level::Verbose,
                "Read Data read from wire. Size: " + std::to_string(numberOfBytesRead) + ".");
          },
          WINHTTP_CALLBACK_STATUS_READ_COMPLETE,
          context))
  {
    GetErrorAndThrow("Error sending HTTP request asynchronously", m_httpAction->GetStowedError());
  }
  if (numberOfBytesRead == 0)
  {
    numberOfBytesRead = m_httpAction->GetBytesAvailable();
  }

  Log::Write(
      Logger::Level::Verbose, "ReadData returned size: " + std::to_string(numberOfBytesRead) + ".");

  return numberOfBytesRead;
}

// Read the response from the sent request.
size_t _detail::WinHttpStream::OnRead(uint8_t* buffer, size_t count, Context const& context)
{
  if (count == 0 || this->m_isEOF)
  {
    return 0;
  }

  size_t numberOfBytesRead = m_requestHandle->ReadData(buffer, count, context);

  this->m_streamTotalRead += numberOfBytesRead;

  if (numberOfBytesRead == 0
      || (this->m_contentLength != -1 && this->m_streamTotalRead == this->m_contentLength))
  {
    this->m_isEOF = true;
  }
  return numberOfBytesRead;
}
