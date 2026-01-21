// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "azure/identity/client_certificate_credential.hpp"

#include "private/tenant_id_resolver.hpp"
#include "private/token_credential_impl.hpp"

#include <azure/core/base64.hpp>
#include <azure/core/datetime.hpp>
#include <azure/core/internal/cryptography/sha_hash.hpp>
#include <azure/core/internal/strings.hpp>
#include <azure/core/io/body_stream.hpp>
#include <azure/core/platform.hpp>
#include <azure/core/uuid.hpp>

#include <chrono>
#include <iomanip>
#include <sstream>
#include <vector>

#if defined(AZ_PLATFORM_WINDOWS)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <wincrypt.h>

#if !defined(WINAPI_PARTITION_DESKTOP) || WINAPI_PARTITION_DESKTOP // not UWP
#pragma warning(push)
#pragma warning(disable : 6553)
#pragma warning(disable : 6001) // Using uninitialized memory 'pNode'.
#pragma warning(disable : 6387) // An argument in result_macros.h may be '0', for the function
                                // 'GetProcAddress'.
#include <wil/resource.h>
#include <wil/result.h>
#pragma warning(pop)
#endif // UWP
#endif

#if !defined(AZ_PLATFORM_WINDOWS) \
    || (defined(WINAPI_PARTITION_DESKTOP) && !WINAPI_PARTITION_DESKTOP)
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/ossl_typ.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#endif

using Azure::Identity::ClientCertificateCredential;

using Azure::Core::Context;
using Azure::Core::Url;
using Azure::Core::Uuid;
using Azure::Core::_internal::Base64Url;
using Azure::Core::_internal::PosixTimeConverter;
using Azure::Core::Credentials::AccessToken;
using Azure::Core::Credentials::AuthenticationException;
using Azure::Core::Credentials::TokenCredentialOptions;
using Azure::Core::Credentials::TokenRequestContext;
using Azure::Core::Http::HttpMethod;
using Azure::Core::IO::FileBodyStream;
using Azure::Identity::_detail::TenantIdResolver;
using Azure::Identity::_detail::TokenCredentialImpl;

namespace {
template <typename T> std::vector<uint8_t> ToUInt8Vector(T const& in)
{
  const size_t size = in.size();
  std::vector<uint8_t> outVec(size);
  for (size_t i = 0; i < size; ++i)
  {
    outVec[i] = static_cast<uint8_t>(in[i]);
  }

  return outVec;
}

std::string FindPemCertificateContent(std::string const& path, std::string clientCertificate)
{
  std::string pem = clientCertificate;
  if (clientCertificate.empty())
  {
    auto pemContent{FileBodyStream(path).ReadToEnd()};
    pem = std::string{pemContent.begin(), pemContent.end()};
    pemContent = {};
  }

  const std::string beginHeader = std::string("-----BEGIN CERTIFICATE-----");
  auto headerStart = pem.find(beginHeader);
  if (headerStart == std::string::npos)
  {
    throw AuthenticationException("PEM file does not contain certificate.");
  }

  auto footerStart = pem.find("-----END CERTIFICATE-----", headerStart);
  if (footerStart == std::string::npos)
  {
    throw AuthenticationException("PEM file does not contain a valid end certificate marker.");
  }

  // Move past the begin marker
  headerStart += beginHeader.length();

  // Extract the certificate without the end marker
  std::string certificate = pem.substr(headerStart, footerStart - headerStart);

  // Remove all new lines
  certificate.erase(std::remove(certificate.begin(), certificate.end(), '\n'), certificate.end());
  certificate.erase(std::remove(certificate.begin(), certificate.end(), '\r'), certificate.end());

  return certificate;
}

using CertificateThumbprint = std::vector<unsigned char>;
using UniquePrivateKey = Azure::Identity::_detail::UniquePrivateKey;
using PrivateKey = decltype(std::declval<UniquePrivateKey>().get());

std::string GetJwtToken(
    CertificateThumbprint mdVec,
    bool sendCertificateChain,
    std::string const& clientCertificatePath,
    std::string clientCertificate = {})
{
  std::string thumbprintHexStr;
  std::string thumbprintBase64Str;

  // Get thumbprint as hex string:
  {
    std::ostringstream thumbprintStream;
    for (const auto md : mdVec)
    {
      thumbprintStream << std::uppercase << std::hex << std::setfill('0') << std::setw(2)
                       << static_cast<int>(md);
    }
    thumbprintHexStr = thumbprintStream.str();
  }

  // Get thumbprint as Base64:
  thumbprintBase64Str = Base64Url::Base64UrlEncode(ToUInt8Vector(mdVec));

  std::string x5cHeaderParam{};
  if (sendCertificateChain)
  {
    // Since there is only one base64 encoded cert string, it can be written as a JSON string rather
    // than a JSON array of strings.
    x5cHeaderParam = ",\"x5c\":\"";
    std::string certContent = FindPemCertificateContent(clientCertificatePath, clientCertificate);
    x5cHeaderParam += certContent;
    x5cHeaderParam += "\"";
  }

  // Form a JWT token:
  const auto tokenHeader = std::string("{\"x5t\":\"") + thumbprintBase64Str + "\",\"kid\":\""
      + thumbprintHexStr + "\",\"alg\":\"RS256\",\"typ\":\"JWT\"" + x5cHeaderParam + "}";

  const auto tokenHeaderVec
      = std::vector<std::string::value_type>(tokenHeader.begin(), tokenHeader.end());

  return Base64Url::Base64UrlEncode(ToUInt8Vector(tokenHeaderVec));
}

#if defined(AZ_PLATFORM_WINDOWS) && (!defined(WINAPI_PARTITION_DESKTOP) || WINAPI_PARTITION_DESKTOP)
enum PrivateKeyType
{
  Rsa,
  Ecdsa,
  Pkcs
};

std::vector<uint8_t> PemToBinary(LPCSTR str, DWORD count)
{
  DWORD size = 0;
  THROW_IF_WIN32_BOOL_FALSE(CryptStringToBinaryA(
      str, count, CRYPT_STRING_BASE64HEADER, nullptr, &size, nullptr, nullptr));
  std::vector<uint8_t> buffer(size);
  THROW_IF_WIN32_BOOL_FALSE(CryptStringToBinaryA(
      str, count, CRYPT_STRING_BASE64HEADER, buffer.data(), &size, nullptr, nullptr));
  return buffer;
}

CertificateThumbprint GetThumbprint(PCCERT_CONTEXT cert)
{
  DWORD size = 0;
  THROW_IF_WIN32_BOOL_FALSE(
      CertGetCertificateContextProperty(cert, CERT_SHA1_HASH_PROP_ID, nullptr, &size));
  std::vector<unsigned char> thumbprint(size);
  THROW_IF_WIN32_BOOL_FALSE(
      CertGetCertificateContextProperty(cert, CERT_SHA1_HASH_PROP_ID, thumbprint.data(), &size));
  return thumbprint;
}

wil::unique_cert_context ImportPemCertificate(std::vector<uint8_t> clientCertificate)
{
  std::string certStr(clientCertificate.begin(), clientCertificate.end());
  auto certBuffer = PemToBinary(certStr.c_str(), static_cast<DWORD>(certStr.size()));
  auto cert = CertCreateCertificateContext(
      X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
      certBuffer.data(),
      static_cast<DWORD>(certBuffer.size()));
  THROW_LAST_ERROR_IF_NULL(cert);
  return wil::unique_cert_context{cert};
}

wil::unique_cert_context ImportPemCertificate(std::string const& pem)
{
  auto headerStart = pem.find("-----BEGIN CERTIFICATE-----");
  if (headerStart == std::string::npos)
  {
    throw AuthenticationException("PEM file does not contain certificate.");
  }
  auto certBuffer
      = PemToBinary(pem.c_str() + headerStart, static_cast<DWORD>(pem.size() - headerStart));
  auto cert = CertCreateCertificateContext(
      X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
      certBuffer.data(),
      static_cast<DWORD>(certBuffer.size()));
  THROW_LAST_ERROR_IF_NULL(cert);
  return wil::unique_cert_context{cert};
}

size_t FindPemPrivateKeyHeader(std::string const& pem, PrivateKeyType& keyType)
{
  auto headerStart = pem.find("-----BEGIN RSA PRIVATE KEY-----");
  if (headerStart != std::string::npos)
  {
    keyType = PrivateKeyType::Rsa;
    return headerStart;
  }
  headerStart = pem.find("-----BEGIN EC PRIVATE KEY-----");
  if (headerStart != std::string::npos)
  {
    keyType = PrivateKeyType::Ecdsa;
    return headerStart;
  }
  keyType = PrivateKeyType::Pkcs;
  return pem.find("-----BEGIN PRIVATE KEY-----");
}

wil::unique_bcrypt_algorithm OpenAlgorithm(LPCWSTR algId)
{
  wil::unique_bcrypt_algorithm alg;
  THROW_IF_NTSTATUS_FAILED(BCryptOpenAlgorithmProvider(wil::out_param(alg), algId, nullptr, 0));
  return alg;
}

UniquePrivateKey ImportRsaPrivateKey(const BYTE* data, DWORD size)
{
  DWORD keySize = 0;
  wil::unique_hlocal_ptr<BCRYPT_RSAKEY_BLOB> rsaKeyBlob;
  THROW_IF_WIN32_BOOL_FALSE(CryptDecodeObjectEx(
      X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
      CNG_RSA_PRIVATE_KEY_BLOB,
      data,
      size,
      CRYPT_DECODE_ALLOC_FLAG,
      nullptr,
      wil::out_param(rsaKeyBlob),
      &keySize));
  BCRYPT_KEY_HANDLE key;
  auto alg = OpenAlgorithm(BCRYPT_RSA_ALGORITHM);
  THROW_IF_NTSTATUS_FAILED(BCryptImportKeyPair(
      alg.get(),
      nullptr,
      BCRYPT_RSAPRIVATE_BLOB,
      &key,
      reinterpret_cast<uint8_t*>(rsaKeyBlob.get()),
      keySize,
      0));
  return UniquePrivateKey{key};
}

UniquePrivateKey ImportEccPrivateKey(const BYTE*, DWORD)
{
  throw AuthenticationException("ECDSA private keys are not supported.");
}

UniquePrivateKey ImportPemPrivateKey(std::string const& pem)
{
  PrivateKeyType keyType{};
  auto headerStart = FindPemPrivateKeyHeader(pem, keyType);
  if (headerStart == std::string::npos)
  {
    throw AuthenticationException("PEM file does not contain private key.");
  }
  auto keyBuffer{
      PemToBinary(pem.c_str() + headerStart, static_cast<DWORD>(pem.size() - headerStart))};

  if (keyType == PrivateKeyType::Rsa)
  {
    return ImportRsaPrivateKey(keyBuffer.data(), static_cast<DWORD>(keyBuffer.size()));
  }

  if (keyType == PrivateKeyType::Ecdsa)
  {
    return ImportEccPrivateKey(keyBuffer.data(), static_cast<DWORD>(keyBuffer.size()));
  }

  wil::unique_hlocal_ptr<CRYPT_PRIVATE_KEY_INFO> privateKeyInfo;
  DWORD keySize = 0;
  THROW_IF_WIN32_BOOL_FALSE(CryptDecodeObjectEx(
      X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
      PKCS_PRIVATE_KEY_INFO,
      keyBuffer.data(),
      static_cast<DWORD>(keyBuffer.size()),
      CRYPT_DECODE_ALLOC_FLAG,
      nullptr,
      wil::out_param(privateKeyInfo),
      &keySize));
  if (strcmp(privateKeyInfo->Algorithm.pszObjId, szOID_RSA_RSA) == 0)
  {
    return ImportRsaPrivateKey(
        privateKeyInfo->PrivateKey.pbData, privateKeyInfo->PrivateKey.cbData);
  }
  if (strcmp(privateKeyInfo->Algorithm.pszObjId, szOID_ECC_PUBLIC_KEY) == 0)
  {
    return ImportEccPrivateKey(
        privateKeyInfo->PrivateKey.pbData, privateKeyInfo->PrivateKey.cbData);
  }
  throw AuthenticationException("Invalid private key.");
}

std::tuple<CertificateThumbprint, UniquePrivateKey> ReadPemCertificate(
    std::string const& clientCertificate,
    std::string const& privateKey)
{
  auto certContext = ImportPemCertificate(clientCertificate);

  // We only support the RSA private key type.
  return std::make_tuple(GetThumbprint(certContext.get()), ImportPemPrivateKey(privateKey));
}

std::tuple<CertificateThumbprint, UniquePrivateKey> ReadPemCertificate(std::string const& path)
{
  auto pemContent{FileBodyStream(path).ReadToEnd()};
  std::string pem{pemContent.begin(), pemContent.end()};
  pemContent = {};

  auto certContext = ImportPemCertificate(pem);
  return std::make_tuple(GetThumbprint(certContext.get()), ImportPemPrivateKey(pem));
}

std::vector<unsigned char> SignPkcs1Sha256(PrivateKey key, const uint8_t* data, size_t size)
{
  auto hash = Azure::Core::Cryptography::_internal::Sha256Hash().Final(data, size);
  BCRYPT_PKCS1_PADDING_INFO paddingInfo;
  paddingInfo.pszAlgId = BCRYPT_SHA256_ALGORITHM;
  DWORD signatureSize = 0;
  auto status = BCryptSignHash(
      key,
      &paddingInfo,
      hash.data(),
      static_cast<ULONG>(hash.size()),
      nullptr,
      0,
      &signatureSize,
      BCRYPT_PAD_PKCS1);
  if (status != ERROR_SUCCESS)
  {
    return {};
  }
  std::vector<unsigned char> signature(signatureSize);
  status = BCryptSignHash(
      key,
      &paddingInfo,
      hash.data(),
      static_cast<ULONG>(hash.size()),
      signature.data(),
      static_cast<ULONG>(signature.size()),
      &signatureSize,
      BCRYPT_PAD_PKCS1);
  if (status != ERROR_SUCCESS)
  {
    return {};
  }
  return signature;
}
#else
template <typename> struct UniqueHandleHelper;

template <> struct UniqueHandleHelper<BIO>
{
  using type = Azure::Core::_internal::BasicUniqueHandle<BIO, BIO_free_all>;
};

template <> struct UniqueHandleHelper<X509>
{
  using type = Azure::Core::_internal::BasicUniqueHandle<X509, X509_free>;
};

template <> struct UniqueHandleHelper<EVP_MD_CTX>
{
  using type = Azure::Core::_internal::BasicUniqueHandle<EVP_MD_CTX, EVP_MD_CTX_free>;
};

template <typename T>
using UniqueHandle = Azure::Core::_internal::UniqueHandle<T, UniqueHandleHelper>;

std::tuple<CertificateThumbprint, UniquePrivateKey> GetThumbprintAndKey(
    UniqueHandle<X509> x509,
    UniquePrivateKey pkey)
{
  CertificateThumbprint thumbprint(EVP_MAX_MD_SIZE);
  // Get certificate thumbprint:
  unsigned int mdLen = 0;
  const auto digestResult = X509_digest(x509.get(), EVP_sha1(), thumbprint.data(), &mdLen);

  if (!digestResult)
  {
    throw AuthenticationException("Failed to get certificate thumbprint.");
  }

  // Drop unused buffer space:
  const auto mdLenSz = static_cast<decltype(thumbprint)::size_type>(mdLen);
  if (thumbprint.size() > mdLenSz)
  {
    thumbprint.resize(mdLenSz);
  }

  return std::make_tuple(thumbprint, std::move(pkey));
}

std::tuple<CertificateThumbprint, UniquePrivateKey> ReadPemCertificate(
    std::string const& clientCertificate,
    std::string const& privateKey)
{
  // Create a BIO from the private key vector data in memory.
  UniqueHandle<BIO> bioKey(BIO_new_mem_buf(privateKey.data(), static_cast<int>(privateKey.size())));
  if (!bioKey)
  {
    throw AuthenticationException("Failed to create BIO for the PEM encoded private key.");
  }

  UniquePrivateKey pkey{PEM_read_bio_PrivateKey(bioKey.get(), nullptr, nullptr, nullptr)};
  if (!pkey)
  {
    throw AuthenticationException(
        "Failed to read the PEM encoded private key for the certificate.");
  }

  // Create a BIO from the client certificate vector data in memory.
  UniqueHandle<BIO> bioCert(
      BIO_new_mem_buf(clientCertificate.data(), static_cast<int>(clientCertificate.size())));
  if (!bioCert)
  {
    throw AuthenticationException("Failed to create BIO for the client certificate.");
  }

  UniqueHandle<X509> x509{PEM_read_bio_X509(bioCert.get(), nullptr, nullptr, nullptr)};
  if (!x509)
  {
    std::ignore = BIO_seek(bioCert.get(), 0);
    x509.reset(PEM_read_bio_X509(bioCert.get(), nullptr, nullptr, nullptr));
    if (!x509)
    {
      throw AuthenticationException("Failed to read X509 section.");
    }
  }

  return GetThumbprintAndKey(std::move(x509), std::move(pkey));
}

std::tuple<CertificateThumbprint, UniquePrivateKey> ReadPemCertificate(const std::string& path)
{
  // Open certificate file, then get private key and X509:
  UniqueHandle<BIO> bio(BIO_new_file(path.c_str(), "r"));
  if (!bio)
  {
    throw AuthenticationException("Failed to open file for reading. File name: '" + path + "'");
  }

  UniquePrivateKey pkey{PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr)};
  if (!pkey)
  {
    throw AuthenticationException("Failed to read certificate private key.");
  }

  UniqueHandle<X509> x509{PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr)};
  if (!x509)
  {
    std::ignore = BIO_seek(bio.get(), 0);
    x509.reset(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr));
    if (!x509)
    {
      throw AuthenticationException("Failed to read X509 section.");
    }
  }

  return GetThumbprintAndKey(std::move(x509), std::move(pkey));
}

std::vector<unsigned char> SignPkcs1Sha256(PrivateKey key, const uint8_t* data, size_t size)
{
  UniqueHandle<EVP_MD_CTX> mdCtx(EVP_MD_CTX_new());
  if (!mdCtx)
  {
    return {};
  }
  EVP_PKEY_CTX* signCtx = nullptr;
  if ((EVP_DigestSignInit(mdCtx.get(), &signCtx, EVP_sha256(), nullptr, static_cast<EVP_PKEY*>(key))
       == 1)
      && (EVP_PKEY_CTX_set_rsa_padding(signCtx, RSA_PKCS1_PADDING) == 1))
  {
    size_t sigLen = 0;
    if (EVP_DigestSign(mdCtx.get(), nullptr, &sigLen, nullptr, 0) == 1)
    {
      std::vector<unsigned char> sigVec(sigLen);
      if (EVP_DigestSign(mdCtx.get(), sigVec.data(), &sigLen, data, size) == 1)
      {
        return sigVec;
      }
    }
  }
  return {};
}
#endif
} // namespace

void Azure::Identity::_detail::FreePrivateKeyImpl(void* pkey)
{
#if defined(AZ_PLATFORM_WINDOWS) && (!defined(WINAPI_PARTITION_DESKTOP) || WINAPI_PARTITION_DESKTOP)
  BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(pkey));
#else
  EVP_PKEY_free(static_cast<EVP_PKEY*>(pkey));
#endif
}

ClientCertificateCredential::ClientCertificateCredential(
    std::string tenantId,
    std::string const& clientId,
    std::string const& clientCertificatePath,
    std::string const& authorityHost,
    std::vector<std::string> additionallyAllowedTenants,
    bool sendCertificateChain,
    Core::Credentials::TokenCredentialOptions const& options)
    : TokenCredential("ClientCertificateCredential"),
      m_clientCredentialCore(tenantId, authorityHost, additionallyAllowedTenants),
      m_tokenCredentialImpl(std::make_unique<TokenCredentialImpl>(options)),
      m_requestBody(
          std::string(
              "grant_type=client_credentials"
              "&client_assertion_type="
              "urn%3Aietf%3Aparams%3Aoauth%3Aclient-assertion-type%3Ajwt-bearer" // cspell:disable-line
              "&client_id=")
          + Url::Encode(clientId)),
      m_tokenPayloadStaticPart(
          "\",\"iss\":\"" + clientId + "\",\"sub\":\"" + clientId + "\",\"jti\":\"")
{
  CertificateThumbprint mdVec;
  try
  {
    if (clientCertificatePath.empty())
    {
      throw AuthenticationException("Certificate file path is empty.");
    }

    using Azure::Core::_internal::StringExtensions;
    std::string const PemExtension = ".pem";
    auto const certFileExtensionStart = clientCertificatePath.find_last_of('.');
    auto const certFileExtension = certFileExtensionStart != std::string::npos
        ? clientCertificatePath.substr(certFileExtensionStart)
        : std::string{};

    if (!StringExtensions::LocaleInvariantCaseInsensitiveEqual(certFileExtension, PemExtension))
    {
      throw AuthenticationException(
          "Certificate format"
          + (certFileExtension.empty() ? " " : " ('" + certFileExtension + "') ")
          + "is not supported. Please convert your certificate to '" + PemExtension + "'.");
    }

    std::tie(mdVec, m_pkey) = ReadPemCertificate(clientCertificatePath);
  }
  catch (std::exception const& e)
  {
    // WIL does not throw AuthenticationException.
    throw AuthenticationException(
        std::string("Identity: ClientCertificateCredential: ") + e.what());
  }

  m_tokenHeaderEncoded = GetJwtToken(mdVec, sendCertificateChain, clientCertificatePath);
}

ClientCertificateCredential::ClientCertificateCredential(
    std::string tenantId,
    std::string const& clientId,
    std::string const& clientCertificate,
    std::string const& privateKey,
    std::string const& authorityHost,
    std::vector<std::string> additionallyAllowedTenants,
    bool sendCertificateChain,
    Core::Credentials::TokenCredentialOptions const& options)
    : TokenCredential("ClientCertificateCredential"),
      m_clientCredentialCore(tenantId, authorityHost, additionallyAllowedTenants),
      m_tokenCredentialImpl(std::make_unique<TokenCredentialImpl>(options)),
      m_requestBody(
          std::string(
              "grant_type=client_credentials"
              "&client_assertion_type="
              "urn%3Aietf%3Aparams%3Aoauth%3Aclient-assertion-type%3Ajwt-bearer" // cspell:disable-line
              "&client_id=")
          + Url::Encode(clientId)),
      m_tokenPayloadStaticPart(
          "\",\"iss\":\"" + clientId + "\",\"sub\":\"" + clientId + "\",\"jti\":\"")
{

  CertificateThumbprint mdVec;
  try
  {
    std::tie(mdVec, m_pkey) = ReadPemCertificate(clientCertificate, privateKey);
  }
  catch (std::exception const& e)
  {
    // WIL does not throw AuthenticationException.
    throw AuthenticationException(
        std::string("Identity: ClientCertificateCredential: ") + e.what());
  }

  m_tokenHeaderEncoded = GetJwtToken(mdVec, sendCertificateChain, {}, clientCertificate);
}

ClientCertificateCredential::ClientCertificateCredential(
    std::string tenantId,
    std::string const& clientId,
    std::string const& clientCertificatePath,
    ClientCertificateCredentialOptions const& options)
    : ClientCertificateCredential(
        tenantId,
        clientId,
        clientCertificatePath,
        options.AuthorityHost,
        options.AdditionallyAllowedTenants,
        options.SendCertificateChain,
        options)
{
}

ClientCertificateCredential::ClientCertificateCredential(
    std::string tenantId,
    std::string const& clientId,
    std::string const& clientCertificatePath,
    Core::Credentials::TokenCredentialOptions const& options)
    : ClientCertificateCredential(
        tenantId,
        clientId,
        clientCertificatePath,
        ClientCertificateCredentialOptions{}.AuthorityHost,
        ClientCertificateCredentialOptions{}.AdditionallyAllowedTenants,
        false, // By default, we don't send the x5c property
        options)
{
}

ClientCertificateCredential::ClientCertificateCredential(
    std::string tenantId,
    std::string const& clientId,
    std::string const& clientCertificate,
    std::string const& privateKey,
    ClientCertificateCredentialOptions const& options)
    : ClientCertificateCredential(
        tenantId,
        clientId,
        clientCertificate,
        privateKey,
        options.AuthorityHost,
        options.AdditionallyAllowedTenants,
        options.SendCertificateChain,
        options)
{
}

ClientCertificateCredential::~ClientCertificateCredential() = default;

AccessToken ClientCertificateCredential::GetToken(
    TokenRequestContext const& tokenRequestContext,
    Context const& context) const
{
  auto const tenantId = TenantIdResolver::Resolve(
      m_clientCredentialCore.GetTenantId(),
      tokenRequestContext,
      m_clientCredentialCore.GetAdditionallyAllowedTenants());

  auto const scopesStr
      = m_clientCredentialCore.GetScopesString(tenantId, tokenRequestContext.Scopes);

  // TokenCache::GetToken() and m_tokenCredentialImpl->GetToken() can only use the lambda argument
  // when they are being executed. They are not supposed to keep a reference to lambda argument to
  // call it later. Therefore, any capture made here will outlive the possible time frame when the
  // lambda might get called.
  return m_tokenCache.GetToken(scopesStr, tenantId, tokenRequestContext.MinimumExpiration, [&]() {
    return m_tokenCredentialImpl->GetToken(context, false, [&]() {
      auto body = m_requestBody;
      if (!scopesStr.empty())
      {
        body += "&scope=" + scopesStr;
      }

      auto const requestUrl = m_clientCredentialCore.GetRequestUrl(tenantId);

      std::string assertion = m_tokenHeaderEncoded;
      {
        // Form the assertion to sign.
        {
          std::string payloadStr;
          // Add GUID, current time, and expiration time to the payload
          {
            // MSAL has JWT token expiration hardcoded as 10 minutes, without further explanations
            // anywhere nearby the constant.
            // https://github.com/AzureAD/microsoft-authentication-library-for-dotnet/blob/01ecd12464007fc1988b6a127aa0b1b980bca1ed/src/client/Microsoft.Identity.Client/Internal/JsonWebTokenConstants.cs#L8
            DateTime const now = std::chrono::system_clock::now();
            DateTime const exp = now + std::chrono::minutes(10);

            payloadStr = std::string("{\"aud\":\"") + requestUrl.GetAbsoluteUrl()
                + m_tokenPayloadStaticPart + Uuid::CreateUuid().ToString()
                + "\",\"nbf\":" + std::to_string(PosixTimeConverter::DateTimeToPosixTime(now))
                + ",\"exp\":" + std::to_string(PosixTimeConverter::DateTimeToPosixTime(exp)) + "}";
          }

          // Concatenate JWT token header + "." + encoded payload
          const auto payloadVec
              = std::vector<std::string::value_type>(payloadStr.begin(), payloadStr.end());

          assertion += std::string(".") + Base64Url::Base64UrlEncode(ToUInt8Vector(payloadVec));
        }

        // Get assertion signature.
        std::string signature = Base64Url::Base64UrlEncode(SignPkcs1Sha256(
            m_pkey.get(),
            reinterpret_cast<const unsigned char*>(assertion.data()),
            static_cast<size_t>(assertion.size())));

        if (signature.empty())
        {
          throw AuthenticationException("Failed to sign token request.");
        }

        // Add signature to the end of assertion
        assertion += std::string(".") + signature;
      }

      body += "&client_assertion=" + Azure::Core::Url::Encode(assertion);

      auto request
          = std::make_unique<TokenCredentialImpl::TokenRequest>(HttpMethod::Post, requestUrl, body);

      request->HttpRequest.SetHeader("Host", requestUrl.GetHost());

      return request;
    });
  });
}
