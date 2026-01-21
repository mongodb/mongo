// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "azure/core/cryptography/hash.hpp"
#include "azure/core/platform.hpp"

#if defined(AZ_PLATFORM_WINDOWS)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

// Windows needs to go before bcrypt
#include <windows.h>

#include <bcrypt.h>
#elif defined(AZ_PLATFORM_POSIX)
#include <openssl/evp.h>
#endif

#include <stdexcept>
#include <vector>

namespace {

#if defined(AZ_PLATFORM_WINDOWS)

class Md5AlgorithmProvider final {
private:
  // Make sure the initial state of status is non-successful
  NTSTATUS m_status = 0;

public:
  BCRYPT_ALG_HANDLE Handle;
  size_t ContextSize;
  size_t HashLength;

  Md5AlgorithmProvider()
  {
    // open an algorithm handle
    if (!BCRYPT_SUCCESS(
            m_status = BCryptOpenAlgorithmProvider(&Handle, BCRYPT_MD5_ALGORITHM, nullptr, 0)))
    {
      throw std::runtime_error("BCryptOpenAlgorithmProvider failed with code: " + m_status);
    }

    // calculate the size of the buffer to hold the hash object
    DWORD objectLength = 0;
    DWORD dataLength = 0;
    if (!BCRYPT_SUCCESS(
            m_status = BCryptGetProperty(
                Handle,
                BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PBYTE>(&objectLength),
                sizeof(objectLength),
                &dataLength,
                0)))
    {
      throw std::runtime_error("BCryptGetProperty failed with code: " + m_status);
    }

    // calculate the length of the hash
    ContextSize = objectLength;
    DWORD hashLength = 0;
    if (!BCRYPT_SUCCESS(
            m_status = BCryptGetProperty(
                Handle,
                BCRYPT_HASH_LENGTH,
                reinterpret_cast<PBYTE>(&hashLength),
                sizeof(hashLength),
                &dataLength,
                0)))
    {
      throw std::runtime_error("BCryptGetProperty failed with code: " + m_status);
    }
    HashLength = hashLength;
  }

  ~Md5AlgorithmProvider()
  {
    if (Handle)
    {
      BCryptCloseAlgorithmProvider(Handle, 0);
    }
  }
};

Md5AlgorithmProvider const& GetMD5AlgorithmProvider()
{
  static Md5AlgorithmProvider instance;
  return instance;
}

class Md5BCrypt final : public Azure::Core::Cryptography::Hash {
private:
  // Make sure the initial state of status is non-successful
  NTSTATUS m_status = 0;
  BCRYPT_HASH_HANDLE m_hashHandle = nullptr;
  size_t m_hashLength = 0;
  std::string m_buffer;

  void OnAppend(const uint8_t* data, size_t length)
  {
    if (!BCRYPT_SUCCESS(
            m_status = BCryptHashData(
                m_hashHandle,
                reinterpret_cast<PBYTE>(const_cast<uint8_t*>(data)),
                static_cast<ULONG>(length),
                0)))
    {
      throw std::runtime_error("BCryptHashData failed with code: " + m_status);
    }
  }

  std::vector<uint8_t> OnFinal(const uint8_t* data, size_t length)
  {
    OnAppend(data, length);

    std::vector<uint8_t> hash;
    hash.resize(m_hashLength);
    if (!BCRYPT_SUCCESS(
            m_status = BCryptFinishHash(
                m_hashHandle,
                reinterpret_cast<PUCHAR>(&hash[0]),
                static_cast<ULONG>(hash.size()),
                0)))
    {
      throw std::runtime_error("BCryptFinishHash failed with code: " + m_status);
    }
    return hash;
  }

public:
  Md5BCrypt()
  {
    m_buffer.resize(GetMD5AlgorithmProvider().ContextSize);
    m_hashLength = GetMD5AlgorithmProvider().HashLength;

    if (!BCRYPT_SUCCESS(
            m_status = BCryptCreateHash(
                GetMD5AlgorithmProvider().Handle,
                &m_hashHandle,
                reinterpret_cast<PUCHAR>(&m_buffer[0]),
                static_cast<ULONG>(m_buffer.size()),
                nullptr,
                0,
                0)))
    {
      throw std::runtime_error("BCryptCreateHash failed with code: " + m_status);
    }
  }

  ~Md5BCrypt()
  {
    if (m_hashHandle)
    {
      BCryptDestroyHash(m_hashHandle);
    }
  }
};

} // namespace
Azure::Core::Cryptography::Md5Hash::Md5Hash() : m_implementation(std::make_unique<Md5BCrypt>()) {}

#elif defined(AZ_PLATFORM_POSIX)

class Md5OpenSSL final : public Azure::Core::Cryptography::Hash {
private:
  EVP_MD_CTX* m_context;

  void OnAppend(const uint8_t* data, size_t length) override
  {
    if (1 != EVP_DigestUpdate(m_context, data, length))
    {
      throw std::runtime_error("Crypto error while updating Md5Hash.");
    }
  }

  std::vector<uint8_t> OnFinal(const uint8_t* data, size_t length) override
  {
    OnAppend(data, length);
    unsigned int size;
    unsigned char hash[EVP_MAX_MD_SIZE];
    if (1 != EVP_DigestFinal(m_context, hash, &size))
    {
      throw std::runtime_error("Crypto error while computing Md5Hash.");
    }

    return std::vector<uint8_t>(std::begin(hash), std::begin(hash) + size);
  }

public:
  Md5OpenSSL()
  {
    m_context = EVP_MD_CTX_new();
    if (m_context == NULL)
    {
      throw std::runtime_error("Crypto error while creating EVP context.");
    }
    if (1 != EVP_DigestInit_ex(m_context, EVP_md5(), NULL))
    {
      throw std::runtime_error("Crypto error while init Md5Hash.");
    }
  }

  ~Md5OpenSSL() { EVP_MD_CTX_free(m_context); }
};

} // namespace
Azure::Core::Cryptography::Md5Hash::Md5Hash() : m_implementation(std::make_unique<Md5OpenSSL>()) {}
#endif

namespace Azure { namespace Core { namespace Cryptography {
  Md5Hash::~Md5Hash() {}

  void Md5Hash::OnAppend(const uint8_t* data, size_t length)
  {
    m_implementation->Append(data, length);
  }

  std::vector<uint8_t> Md5Hash::OnFinal(const uint8_t* data, size_t length)
  {
    return m_implementation->Final(data, length);
  }

}}} // namespace Azure::Core::Cryptography
