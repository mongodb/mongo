// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief HTTP status code definition.
 */

#pragma once

#include <string>

namespace Azure { namespace Core { namespace Http {
  /**
   * @brief Defines the possible HTTP status codes.
   */
  enum class HttpStatusCode
  {
    /// No HTTP status code.
    None = 0,

    // === 1xx (information) Status Codes: ===
    Continue = 100, ///< HTTP 100 Continue.
    SwitchingProtocols = 101, ///< HTTP 101 Switching Protocols.
    Processing = 102, ///< HTTP 102 Processing.
    EarlyHints = 103, ///< HTTP 103 Early Hints.

    // === 2xx (successful) Status Codes: ===
    Ok = 200, ///< HTTP 200 OK.
    Created = 201, ///< HTTP 201 Created.
    Accepted = 202, ///< HTTP 202 Accepted.
    NonAuthoritativeInformation = 203, ///< HTTP 203 Non-Authoritative Information.
    NoContent = 204, ///< HTTP 204 No Content.
    ResetContent = 205, ///< HTTP 205 Rest Content.
    PartialContent = 206, ///< HTTP 206 Partial Content.
    MultiStatus = 207, ///< HTTP 207 Multi-Status.
    AlreadyReported = 208, ///< HTTP 208 Already Reported.
    IMUsed = 226, ///< HTTP 226 IM Used.

    // === 3xx (redirection) Status Codes: ===
    MultipleChoices = 300, ///< HTTP 300 Multiple Choices.
    MovedPermanently = 301, ///< HTTP 301 Moved Permanently.
    Found = 302, ///< HTTP 302 Found.
    SeeOther = 303, ///< HTTP 303 See Other.
    NotModified = 304, ///< HTTP 304 Not Modified.
    UseProxy = 305, ///< HTTP 305 Use Proxy.
    TemporaryRedirect = 307, ///< HTTP 307 Temporary Redirect.
    PermanentRedirect = 308, ///< HTTP 308 Permanent Redirect.

    // === 4xx (client error) Status Codes: ===
    BadRequest = 400, ///< HTTP 400 Bad Request.
    Unauthorized = 401, ///< HTTP 401 Unauthorized.
    PaymentRequired = 402, ///< HTTP 402 Payment Required.
    Forbidden = 403, ///< HTTP 403 Forbidden.
    NotFound = 404, ///< HTTP 404 Not Found.
    MethodNotAllowed = 405, ///< HTTP 405 Method Not Allowed.
    NotAcceptable = 406, ///< HTTP 406 Not Acceptable.
    ProxyAuthenticationRequired = 407, ///< HTTP 407 Proxy Authentication Required.
    RequestTimeout = 408, ///< HTTP 408 Request Timeout.
    Conflict = 409, ///< HTTP 409 Conflict.
    Gone = 410, ///< HTTP 410 Gone.
    LengthRequired = 411, ///< HTTP 411 Length Required.
    PreconditionFailed = 412, ///< HTTP 412 Precondition Failed.
    PayloadTooLarge = 413, ///< HTTP 413 Payload Too Large.
    UriTooLong = 414, ///< HTTP 414 URI Too Long.
    UnsupportedMediaType = 415, ///< HTTP 415 Unsupported Media Type.
    RangeNotSatisfiable = 416, ///< HTTP 416 Range Not Satisfiable.
    ExpectationFailed = 417, ///< HTTP 417 Expectation Failed.
    MisdirectedRequest = 421, ///< HTTP 421 Misdirected Request.
    UnprocessableEntity = 422, ///< HTTP 422 Unprocessable Entity.
    Locked = 423, ///< HTTP 423 Locked.
    FailedDependency = 424, ///< HTTP 424 Failed Dependency.
    TooEarly = 425, ///< HTTP 425 Too Early.
    UpgradeRequired = 426, ///< HTTP 426 Upgrade Required.
    PreconditionRequired = 428, ///< HTTP 428 Precondition Required.
    TooManyRequests = 429, ///< HTTP 429 Too Many Requests.
    RequestHeaderFieldsTooLarge = 431, ///< HTTP 431 Request Header Fields Too Large.
    UnavailableForLegalReasons = 451, ///< HTTP 451 Unavailable For Legal Reasons.

    // === 5xx (server error) Status Codes: ===
    InternalServerError = 500, ///< HTTP 500 Internal Server Error.
    NotImplemented = 501, ///< HTTP 501 Not Implemented.
    BadGateway = 502, ///< HTTP 502 Bad Gateway.
    ServiceUnavailable = 503, ///< HTTP 503 Unavailable.
    GatewayTimeout = 504, ///< HTTP 504 Gateway Timeout.
    HttpVersionNotSupported = 505, ///< HTTP 505 HTTP Version Not Supported.
    VariantAlsoNegotiates = 506, ///< HTTP 506 Variant Also Negotiates.
    InsufficientStorage = 507, ///< HTTP 507 Insufficient Storage.
    LoopDetected = 508, ///< HTTP 508 Loop Detected.
    NotExtended = 510, ///< HTTP 510 Not Extended.
    NetworkAuthenticationRequired = 511, ///< HTTP 511 Network Authentication Required.
  };

}}} // namespace Azure::Core::Http
