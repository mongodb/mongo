/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/utils/crypto/CRC32.h>
#include <aws/core/utils/crypto/CRC64.h>
#include <aws/core/utils/crypto/Factories.h>
#include <aws/core/utils/crypto/HMAC.h>
#include <aws/core/utils/crypto/Hash.h>
#ifndef NO_ENCRYPTION
#include <aws/core/utils/crypto/crt/CRTSymmetricCipher.h>
#include <aws/core/utils/crypto/crt/CRTHash.h>
#include <aws/core/utils/crypto/crt/CRTHMAC.h>
#include <aws/core/utils/crypto/crt/CRTSecureRandom.h>
#else
// if you don't have any encryption you still need to pull in the interface definitions
#include <aws/core/utils/crypto/Hash.h>
#include <aws/core/utils/crypto/HMAC.h>
#include <aws/core/utils/crypto/Cipher.h>
#include <aws/core/utils/crypto/SecureRandom.h>
#define NO_ENCRYPTION
#endif

using namespace Aws::Utils;
using namespace Aws::Utils::Crypto;

static const char *s_allocationTag = "CryptoFactory";

static std::shared_ptr<HashFactory>& GetMD5Factory()
{
    static std::shared_ptr<HashFactory> s_MD5Factory(nullptr);
    return s_MD5Factory;
}

static std::shared_ptr<HashFactory>& GetCRC32Factory()
{
    static std::shared_ptr<HashFactory> s_CRC32Factory(nullptr);
    return s_CRC32Factory;
}

static std::shared_ptr<HashFactory>& GetCRC32CFactory()
{
    static std::shared_ptr<HashFactory> s_CRC32CFactory(nullptr);
    return s_CRC32CFactory;
}

static std::shared_ptr<HashFactory> &GetCRC64Factory() {
  static std::shared_ptr<HashFactory> s_CRC64Factory(nullptr);
  return s_CRC64Factory;
}

static std::shared_ptr<HashFactory> &GetSha1Factory() {
  static std::shared_ptr<HashFactory> s_Sha1Factory(nullptr);
  return s_Sha1Factory;
}

static std::shared_ptr<HashFactory>& GetSha256Factory()
{
    static std::shared_ptr<HashFactory> s_Sha256Factory(nullptr);
    return s_Sha256Factory;
}

static std::shared_ptr<HMACFactory>& GetSha256HMACFactory()
{
    static std::shared_ptr<HMACFactory> s_Sha256HMACFactory(nullptr);
    return s_Sha256HMACFactory;
}

static std::shared_ptr<SymmetricCipherFactory>& GetAES_CBCFactory()
{
    static std::shared_ptr<SymmetricCipherFactory> s_AES_CBCFactory(nullptr);
    return s_AES_CBCFactory;
}

static std::shared_ptr<SymmetricCipherFactory>& GetAES_CTRFactory()
{
    static std::shared_ptr<SymmetricCipherFactory> s_AES_CTRFactory(nullptr);
    return s_AES_CTRFactory;
}

static std::shared_ptr<SymmetricCipherFactory>& GetAES_GCMFactory()
{
    static std::shared_ptr<SymmetricCipherFactory> s_AES_GCMFactory(nullptr);
    return s_AES_GCMFactory;
}

static std::shared_ptr<SymmetricCipherFactory>& GetAES_KeyWrapFactory()
{
    static std::shared_ptr<SymmetricCipherFactory> s_AES_KeyWrapFactory(nullptr);
    return s_AES_KeyWrapFactory;
}

static std::shared_ptr<SecureRandomFactory>& GetSecureRandomFactory()
{
    static std::shared_ptr<SecureRandomFactory> s_SecureRandomFactory(nullptr);
    return s_SecureRandomFactory;
}

static std::shared_ptr<SecureRandomBytes>& GetSecureRandom()
{
    static std::shared_ptr<SecureRandomBytes> s_SecureRandom(nullptr);
    return s_SecureRandom;
}

static bool s_InitCleanupOpenSSLFlag(false);

class DefaultMD5Factory : public HashFactory
{
public:
    std::shared_ptr<Hash> CreateImplementation() const override
    {
#ifndef NO_ENCRYPTION
        return Aws::MakeShared<CRTHash>(s_allocationTag, Aws::Crt::Crypto::Hash::CreateMD5());
#else
        return nullptr;
#endif /* NO_ENCRYPTION */
    }

    /**
     * Opportunity to make any static initialization calls you need to make.
     * Will only be called once.
     */
    void InitStaticState() override
    {
        // No-op for backwards compatibility.
    }

    /**
     * Opportunity to make any static cleanup calls you need to make.
     * will only be called at the end of the application.
     */
    void CleanupStaticState() override
    {
        // No-op for backwards compatibility.
    }
};

class DefaultCRC32Factory : public HashFactory
{
public:
    std::shared_ptr<Hash> CreateImplementation() const override
    {
        return Aws::MakeShared<CRC32Impl>(s_allocationTag);
    }
};

class DefaultCRC32CFactory : public HashFactory
{
public:
    std::shared_ptr<Hash> CreateImplementation() const override
    {
        return Aws::MakeShared<CRC32CImpl>(s_allocationTag);
    }
};

class DefaultCRC64Factory : public HashFactory {
public:
  std::shared_ptr<Hash> CreateImplementation() const override {
    return Aws::MakeShared<CRC64Impl>(s_allocationTag);
  }
};

class DefaultSHA1Factory : public HashFactory {
public:
  std::shared_ptr<Hash> CreateImplementation() const override {
#ifndef NO_ENCRYPTION
        return Aws::MakeShared<CRTHash>(s_allocationTag, Aws::Crt::Crypto::Hash::CreateSHA1());
#else
        return nullptr;
#endif
  }

    /**
     * Opportunity to make any static initialization calls you need to make.
     * Will only be called once.
     */
    void InitStaticState() override
    {
        // No-op for backwards compatibility.
    }

    /**
     * Opportunity to make any static cleanup calls you need to make.
     * will only be called at the end of the application.
     */
    void CleanupStaticState() override
    {
        // No-op for backwards compatibility.
    }
};

class DefaultSHA256Factory : public HashFactory
{
public:
    std::shared_ptr<Hash> CreateImplementation() const override
    {
#ifndef NO_ENCRYPTION
        return Aws::MakeShared<CRTHash>(s_allocationTag, Aws::Crt::Crypto::Hash::CreateSHA256());
#else
        return nullptr;
#endif
    }

    /**
     * Opportunity to make any static initialization calls you need to make.
     * Will only be called once.
     */
    void InitStaticState() override
    {
        // No-op for backwards compatibility.
    }

    /**
     * Opportunity to make any static cleanup calls you need to make.
     * will only be called at the end of the application.
     */
    void CleanupStaticState() override
    {
        // No-op for backwards compatibility.
    }
};

class DefaultSHA256HmacFactory : public HMACFactory
{
public:
    std::shared_ptr<Aws::Utils::Crypto::HMAC> CreateImplementation() const override
    {
#ifndef NO_ENCRYPTION
        return Aws::MakeShared<CRTSha256Hmac>(s_allocationTag);
#else
        return nullptr;
#endif
    }

    /**
     * Opportunity to make any static initialization calls you need to make.
     * Will only be called once.
     */
    void InitStaticState() override
    {
        // No-op for backwards compatibility.
    }

    /**
     * Opportunity to make any static cleanup calls you need to make.
     * will only be called at the end of the application.
     */
    void CleanupStaticState() override
    {
        // No-op for backwards compatibility.
    }
};


class DefaultAES_CBCFactory : public SymmetricCipherFactory
{
public:
    std::shared_ptr<SymmetricCipher> CreateImplementation(const CryptoBuffer& key) const override
    {
#ifndef NO_ENCRYPTION
        const auto keyCur = Aws::Crt::ByteCursorFromArray(key.GetUnderlyingData(), key.GetLength());
        return Aws::MakeShared<CRTSymmetricCipher>(s_allocationTag, Aws::Crt::Crypto::SymmetricCipher::CreateAES_256_CBC_Cipher(keyCur));
#else
        AWS_UNREFERENCED_PARAM(key);
        return nullptr;
#endif
    }
    /**
     * Factory method. Returns cipher implementation. See the SymmetricCipher class for more details.
     */
    std::shared_ptr<SymmetricCipher> CreateImplementation(const CryptoBuffer& key, const CryptoBuffer& iv, const CryptoBuffer&, const CryptoBuffer&) const override
    {
#ifndef NO_ENCRYPTION
        const auto keyCur = Aws::Crt::ByteCursorFromArray(key.GetUnderlyingData(), key.GetLength());
        const auto ivCur = Aws::Crt::ByteCursorFromArray(iv.GetUnderlyingData(), iv.GetLength());
        return Aws::MakeShared<CRTSymmetricCipher>(s_allocationTag, Aws::Crt::Crypto::SymmetricCipher::CreateAES_256_CBC_Cipher(keyCur, ivCur));
#else
        AWS_UNREFERENCED_PARAM(key);
        AWS_UNREFERENCED_PARAM(iv);
        return nullptr;
#endif
    }
    /**
     * Factory method. Returns cipher implementation. See the SymmetricCipher class for more details.
     */
    std::shared_ptr<SymmetricCipher> CreateImplementation(CryptoBuffer&& key, CryptoBuffer&& iv, CryptoBuffer&& tag, CryptoBuffer&&aad) const override
    {
        return CreateImplementation(key, iv, tag, aad);
    }

    /**
     * Opportunity to make any static initialization calls you need to make.
     * Will only be called once.
     */
    void InitStaticState() override
    {
        // No-op for backwards compatibility.
    }

    /**
     * Opportunity to make any static cleanup calls you need to make.
     * will only be called at the end of the application.
     */
    void CleanupStaticState() override
    {
        // No-op for backwards compatibility.
    }
};

class DefaultAES_CTRFactory : public SymmetricCipherFactory
{
public:
    std::shared_ptr<SymmetricCipher> CreateImplementation(const CryptoBuffer& key) const override
    {
#ifndef NO_ENCRYPTION
        const auto keyCur = Aws::Crt::ByteCursorFromArray(key.GetUnderlyingData(), key.GetLength());
        return Aws::MakeShared<CRTSymmetricCipher>(s_allocationTag, Aws::Crt::Crypto::SymmetricCipher::CreateAES_256_CTR_Cipher(keyCur));
#else
        AWS_UNREFERENCED_PARAM(key);
        return nullptr;
#endif
    }
    /**
     * Factory method. Returns cipher implementation. See the SymmetricCipher class for more details.
     */
    std::shared_ptr<SymmetricCipher> CreateImplementation(const CryptoBuffer& key, const CryptoBuffer& iv, const CryptoBuffer&, const CryptoBuffer&) const override
    {
#ifndef NO_ENCRYPTION
        const auto keyCur = Aws::Crt::ByteCursorFromArray(key.GetUnderlyingData(), key.GetLength());
        const auto ivCur = Aws::Crt::ByteCursorFromArray(iv.GetUnderlyingData(), iv.GetLength());
        return Aws::MakeShared<CRTSymmetricCipher>(s_allocationTag, Aws::Crt::Crypto::SymmetricCipher::CreateAES_256_CTR_Cipher(keyCur, ivCur));
#else
        AWS_UNREFERENCED_PARAM(key);
        AWS_UNREFERENCED_PARAM(iv);
        return nullptr;
#endif
    }
    /**
     * Factory method. Returns cipher implementation. See the SymmetricCipher class for more details.
     */
    std::shared_ptr<SymmetricCipher> CreateImplementation(CryptoBuffer&& key, CryptoBuffer&& iv, CryptoBuffer&& tag, CryptoBuffer&& aad) const override
    {
        return CreateImplementation(key, iv, tag, aad);
    }

    /**
     * Opportunity to make any static initialization calls you need to make.
     * Will only be called once.
     */
    void InitStaticState() override
    {
        // No-op for backwards compatibility.
    }

    /**
     * Opportunity to make any static cleanup calls you need to make.
     * will only be called at the end of the application.
     */
    void CleanupStaticState() override
    {
        // No-op for backwards compatibility.
    }
};

class DefaultAES_GCMFactory : public SymmetricCipherFactory
{
public:
    std::shared_ptr<SymmetricCipher> CreateImplementation(const CryptoBuffer& key) const override
    {
#ifndef NO_ENCRYPTION
        const auto keyCur = Aws::Crt::ByteCursorFromArray(key.GetUnderlyingData(), key.GetLength());
        return Aws::MakeShared<CRTSymmetricCipher>(s_allocationTag, Aws::Crt::Crypto::SymmetricCipher::CreateAES_256_GCM_Cipher(keyCur));
#else
        AWS_UNREFERENCED_PARAM(key);
        return nullptr;
#endif
    }

    std::shared_ptr<SymmetricCipher> CreateImplementation(const CryptoBuffer& key, const CryptoBuffer* aad) const override
    {
#ifndef NO_ENCRYPTION
        auto keyCur = Aws::Crt::ByteCursorFromArray(key.GetUnderlyingData(), key.GetLength());

        if (aad)
        {
            auto aadCur = Aws::Crt::ByteCursorFromArray(aad->GetUnderlyingData(), aad->GetLength());
            const auto cipher = Aws::MakeShared<CRTSymmetricCipher>(s_allocationTag,
                Aws::Crt::Crypto::SymmetricCipher::CreateAES_256_GCM_Cipher(keyCur,
                    Aws::Crt::Optional<Aws::Crt::ByteCursor>(),
                    aadCur));
            return cipher;
        }

        return Aws::MakeShared<CRTSymmetricCipher>(s_allocationTag, Aws::Crt::Crypto::SymmetricCipher::CreateAES_256_GCM_Cipher(keyCur));
#else
        AWS_UNREFERENCED_PARAM(key);
        AWS_UNREFERENCED_PARAM(aad);
        return nullptr;
#endif
    }

    /**
     * Factory method. Returns cipher implementation. See the SymmetricCipher class for more details.
     */
    std::shared_ptr<SymmetricCipher> CreateImplementation(const CryptoBuffer& key, const CryptoBuffer& iv, const CryptoBuffer& tag, const CryptoBuffer& aad) const override
    {
#ifndef NO_ENCRYPTION
        Aws::Crt::Optional<Aws::Crt::ByteCursor> keyCur = key.GetLength() > 0 ? Aws::Crt::ByteCursorFromArray(key.GetUnderlyingData(), key.GetLength()) : Aws::Crt::Optional<Aws::Crt::ByteCursor>();
        Aws::Crt::Optional<Aws::Crt::ByteCursor> ivCur = iv.GetLength() > 0 ? Aws::Crt::ByteCursorFromArray(iv.GetUnderlyingData(), iv.GetLength()) : Aws::Crt::Optional<Aws::Crt::ByteCursor>();
        Aws::Crt::Optional<Aws::Crt::ByteCursor> tagCur = tag.GetLength() > 0 ? Aws::Crt::ByteCursorFromArray(tag.GetUnderlyingData(), tag.GetLength()) : Aws::Crt::Optional<Aws::Crt::ByteCursor>();
        Aws::Crt::Optional<Aws::Crt::ByteCursor> aadCur = aad.GetLength() > 0 ? Aws::Crt::ByteCursorFromArray(aad.GetUnderlyingData(), aad.GetLength()) : Aws::Crt::Optional<Aws::Crt::ByteCursor>();

        auto cipher = Aws::Crt::Crypto::SymmetricCipher::CreateAES_256_GCM_Cipher(keyCur, ivCur, aadCur);
        if (cipher && tagCur.has_value())
        {
            cipher.SetTag(tagCur.value());
        }
        return Aws::MakeShared<CRTSymmetricCipher>(s_allocationTag, std::move(cipher));
#else
        AWS_UNREFERENCED_PARAM(key);
        AWS_UNREFERENCED_PARAM(iv);
        AWS_UNREFERENCED_PARAM(tag);
        AWS_UNREFERENCED_PARAM(aad);
        return nullptr;
#endif
    }
    /**
     * Factory method. Returns cipher implementation. See the SymmetricCipher class for more details.
     */
    std::shared_ptr<SymmetricCipher> CreateImplementation(CryptoBuffer&& key, CryptoBuffer&& iv, CryptoBuffer&& tag, CryptoBuffer&& aad) const override
    {
        return CreateImplementation(key, iv, tag, aad);
    }

    /**
     * Opportunity to make any static initialization calls you need to make.
     * Will only be called once.
     */
    void InitStaticState() override
    {
        // No-op for backwards compatibility.
    }

    /**
     * Opportunity to make any static cleanup calls you need to make.
     * will only be called at the end of the application.
     */
    void CleanupStaticState() override
    {
        // No-op for backwards compatibility.
    }
};

class DefaultAES_KeyWrapFactory : public SymmetricCipherFactory
{
public:
    std::shared_ptr<SymmetricCipher> CreateImplementation(const CryptoBuffer& key) const override
    {
#ifndef NO_ENCRYPTION
        const auto keyCur = Aws::Crt::ByteCursorFromArray(key.GetUnderlyingData(), key.GetLength());
        return Aws::MakeShared<CRTSymmetricCipher>(s_allocationTag, Aws::Crt::Crypto::SymmetricCipher::CreateAES_256_KeyWrap_Cipher(keyCur));
#else
        AWS_UNREFERENCED_PARAM(key);
        return nullptr;
#endif
    }
    /**
    * Factory method. Returns cipher implementation. See the SymmetricCipher class for more details.
    */
    std::shared_ptr<SymmetricCipher> CreateImplementation(const CryptoBuffer& key, const CryptoBuffer& iv, const CryptoBuffer& tag, const CryptoBuffer&) const override
    {
        AWS_UNREFERENCED_PARAM(key);
        AWS_UNREFERENCED_PARAM(iv);
        AWS_UNREFERENCED_PARAM(tag);
        return nullptr;
    }
    /**
    * Factory method. Returns cipher implementation. See the SymmetricCipher class for more details.
    */
    std::shared_ptr<SymmetricCipher> CreateImplementation(CryptoBuffer&& key, CryptoBuffer&& iv, CryptoBuffer&& tag, CryptoBuffer&&) const override
    {
        AWS_UNREFERENCED_PARAM(key);
        AWS_UNREFERENCED_PARAM(iv);
        AWS_UNREFERENCED_PARAM(tag);
        return nullptr;
    }

    /**
    * Opportunity to make any static initialization calls you need to make.
    * Will only be called once.
    */
    void InitStaticState() override
    {
        // No-op for backwards compatibility.
    }

    /**
    * Opportunity to make any static cleanup calls you need to make.
    * will only be called at the end of the application.
    */
    void CleanupStaticState() override
    {
        // No-op for backwards compatibility.
    }
};

class DefaultSecureRandFactory : public SecureRandomFactory
{
    /**
     * Factory method. Returns SecureRandom implementation.
     */
    std::shared_ptr<SecureRandomBytes> CreateImplementation() const override
    {
#ifndef NO_ENCRYPTION
        return Aws::MakeShared<CRTSecureRandomBytes>(s_allocationTag);
#else
        return nullptr;
#endif
    }

    /**
 * Opportunity to make any static initialization calls you need to make.
 * Will only be called once.
 */
    void InitStaticState() override
    {
        // No-op for backwards compatibility.
    }

    /**
     * Opportunity to make any static cleanup calls you need to make.
     * will only be called at the end of the application.
     */
    void CleanupStaticState() override
    {
        // No-op for backwards compatibility.
    }
};

void Aws::Utils::Crypto::SetInitCleanupOpenSSLFlag(bool initCleanupFlag)
{
    s_InitCleanupOpenSSLFlag = initCleanupFlag;
}

void Aws::Utils::Crypto::InitCrypto()
{
    if(GetMD5Factory())
    {
        GetMD5Factory()->InitStaticState();
    }
    else
    {
        GetMD5Factory() = Aws::MakeShared<DefaultMD5Factory>(s_allocationTag);
        GetMD5Factory()->InitStaticState();
    }

    if(!GetCRC32Factory())
    {
        GetCRC32Factory() = Aws::MakeShared<DefaultCRC32Factory>(s_allocationTag);
    }

    if(!GetCRC32CFactory())
    {
        GetCRC32CFactory() = Aws::MakeShared<DefaultCRC32CFactory>(s_allocationTag);
    }

    if (!GetCRC64Factory()) {
      GetCRC64Factory() = Aws::MakeShared<DefaultCRC64Factory>(s_allocationTag);
    }

    if (GetSha1Factory()) {
      GetSha1Factory()->InitStaticState();
    } else {
      GetSha1Factory() = Aws::MakeShared<DefaultSHA1Factory>(s_allocationTag);
      GetSha1Factory()->InitStaticState();
    }

    if(GetSha256Factory())
    {
        GetSha256Factory()->InitStaticState();
    }
    else
    {
        GetSha256Factory() = Aws::MakeShared<DefaultSHA256Factory>(s_allocationTag);
        GetSha256Factory()->InitStaticState();
    }

    if(GetSha256HMACFactory())
    {
        GetSha256HMACFactory()->InitStaticState();
    }
    else
    {
        GetSha256HMACFactory() = Aws::MakeShared<DefaultSHA256HmacFactory>(s_allocationTag);
        GetSha256HMACFactory()->InitStaticState();
    }

    if(GetAES_CBCFactory())
    {
        GetAES_CBCFactory()->InitStaticState();
    }
    else
    {
        GetAES_CBCFactory() = Aws::MakeShared<DefaultAES_CBCFactory>(s_allocationTag);
        GetAES_CBCFactory()->InitStaticState();
    }

    if(GetAES_CTRFactory())
    {
        GetAES_CTRFactory()->InitStaticState();
    }
    else
    {
        GetAES_CTRFactory() = Aws::MakeShared<DefaultAES_CTRFactory>(s_allocationTag);
        GetAES_CTRFactory()->InitStaticState();
    }

    if(GetAES_GCMFactory())
    {
        GetAES_GCMFactory()->InitStaticState();
    }
    else
    {
        GetAES_GCMFactory() = Aws::MakeShared<DefaultAES_GCMFactory>(s_allocationTag);
        GetAES_GCMFactory()->InitStaticState();
    }

    if (!GetAES_KeyWrapFactory())
    {
        GetAES_KeyWrapFactory() = Aws::MakeShared<DefaultAES_KeyWrapFactory>(s_allocationTag);
    }
    GetAES_KeyWrapFactory()->InitStaticState();

    if(GetSecureRandomFactory())
    {
        GetSecureRandomFactory()->InitStaticState();
    }
    else
    {
        GetSecureRandomFactory() = Aws::MakeShared<DefaultSecureRandFactory>(s_allocationTag);
        GetSecureRandomFactory()->InitStaticState();
    }

    GetSecureRandom() = GetSecureRandomFactory()->CreateImplementation();
}

void Aws::Utils::Crypto::CleanupCrypto()
{
    if(GetMD5Factory())
    {
        GetMD5Factory()->CleanupStaticState();
        GetMD5Factory() = nullptr;
    }

    if(GetCRC32CFactory())
    {
        GetCRC32Factory() = nullptr;
    }

    if(GetCRC32CFactory())
    {
        GetCRC32CFactory() = nullptr;
    }

    if (GetCRC64Factory()) {
      GetCRC64Factory() = nullptr;
    }

    if (GetSha1Factory()) {
      GetSha1Factory()->CleanupStaticState();
      GetSha1Factory() = nullptr;
    }

    if(GetSha256Factory())
    {
        GetSha256Factory()->CleanupStaticState();
        GetSha256Factory() = nullptr;
    }

    if(GetSha256HMACFactory())
    {
        GetSha256HMACFactory()->CleanupStaticState();
        GetSha256HMACFactory() =  nullptr;
    }

    if(GetAES_CBCFactory())
    {
        GetAES_CBCFactory()->CleanupStaticState();
        GetAES_CBCFactory() = nullptr;
    }

    if(GetAES_CTRFactory())
    {
        GetAES_CTRFactory()->CleanupStaticState();
        GetAES_CTRFactory() = nullptr;
    }

    if(GetAES_GCMFactory())
    {
        GetAES_GCMFactory()->CleanupStaticState();
        GetAES_GCMFactory() = nullptr;
    }

    if(GetAES_KeyWrapFactory())
    {
        GetAES_KeyWrapFactory()->CleanupStaticState();
        GetAES_KeyWrapFactory() = nullptr;
    }

    if(GetSecureRandomFactory())
    {
        GetSecureRandom() = nullptr;
        GetSecureRandomFactory()->CleanupStaticState();
        GetSecureRandomFactory() = nullptr;
    }
}

void Aws::Utils::Crypto::SetMD5Factory(const std::shared_ptr<HashFactory>& factory)
{
    GetMD5Factory() = factory;
}

void Aws::Utils::Crypto::SetCRC32Factory(const std::shared_ptr<HashFactory>& factory)
{
    GetCRC32Factory() = factory;
}

void Aws::Utils::Crypto::SetCRC64Factory(
    const std::shared_ptr<HashFactory> &factory) {
  GetCRC64Factory() = factory;
}

void Aws::Utils::Crypto::SetCRC32CFactory(
    const std::shared_ptr<HashFactory> &factory) {
  GetCRC32CFactory() = factory;
}

void Aws::Utils::Crypto::SetSha1Factory(
    const std::shared_ptr<HashFactory> &factory) {
  GetSha1Factory() = factory;
}

void Aws::Utils::Crypto::SetSha256Factory(const std::shared_ptr<HashFactory>& factory)
{
    GetSha256Factory() = factory;
}

void Aws::Utils::Crypto::SetSha256HMACFactory(const std::shared_ptr<HMACFactory>& factory)
{
    GetSha256HMACFactory() = factory;
}

void Aws::Utils::Crypto::SetAES_CBCFactory(const std::shared_ptr<SymmetricCipherFactory>& factory)
{
    GetAES_CBCFactory() = factory;
}

void Aws::Utils::Crypto::SetAES_CTRFactory(const std::shared_ptr<SymmetricCipherFactory>& factory)
{
    GetAES_CTRFactory() = factory;
}

void Aws::Utils::Crypto::SetAES_GCMFactory(const std::shared_ptr<SymmetricCipherFactory>& factory)
{
    GetAES_GCMFactory() = factory;
}

void Aws::Utils::Crypto::SetAES_KeyWrapFactory(const std::shared_ptr<SymmetricCipherFactory>& factory)
{
    GetAES_KeyWrapFactory() = factory;
}

void Aws::Utils::Crypto::SetSecureRandomFactory(const std::shared_ptr<SecureRandomFactory>& factory)
{
    GetSecureRandomFactory() = factory;
}

std::shared_ptr<Hash> Aws::Utils::Crypto::CreateMD5Implementation()
{
    return GetMD5Factory()->CreateImplementation();
}

std::shared_ptr<Hash> Aws::Utils::Crypto::CreateCRC32Implementation()
{
    return GetCRC32Factory()->CreateImplementation();
}

std::shared_ptr<Hash> Aws::Utils::Crypto::CreateCRC32CImplementation()
{
    return GetCRC32CFactory()->CreateImplementation();
}

std::shared_ptr<Hash> Aws::Utils::Crypto::CreateCRC64Implementation() {
  return GetCRC64Factory()->CreateImplementation();
}

std::shared_ptr<Hash> Aws::Utils::Crypto::CreateSha1Implementation()
{
  return GetSha1Factory()->CreateImplementation();
}

std::shared_ptr<Hash> Aws::Utils::Crypto::CreateSha256Implementation() {
  return GetSha256Factory()->CreateImplementation();
}

std::shared_ptr<Aws::Utils::Crypto::HMAC> Aws::Utils::Crypto::CreateSha256HMACImplementation()
{
    return GetSha256HMACFactory()->CreateImplementation();
}

#ifdef _WIN32
#pragma warning( push )
#pragma warning( disable : 4702 )
#endif

std::shared_ptr<SymmetricCipher> Aws::Utils::Crypto::CreateAES_CBCImplementation(const CryptoBuffer& key)
{
#ifdef NO_SYMMETRIC_ENCRYPTION
    return nullptr;
#endif
    return GetAES_CBCFactory()->CreateImplementation(key);
}

std::shared_ptr<SymmetricCipher> Aws::Utils::Crypto::CreateAES_CBCImplementation(const CryptoBuffer& key, const CryptoBuffer& iv)
{
#ifdef NO_SYMMETRIC_ENCRYPTION
    return nullptr;
#endif
    return GetAES_CBCFactory()->CreateImplementation(key, iv);
}

std::shared_ptr<SymmetricCipher> Aws::Utils::Crypto::CreateAES_CBCImplementation(CryptoBuffer&& key, CryptoBuffer&& iv)
{
#ifdef NO_SYMMETRIC_ENCRYPTION
    return nullptr;
#endif
    return GetAES_CBCFactory()->CreateImplementation(std::move(key), std::move(iv));
}

std::shared_ptr<SymmetricCipher> Aws::Utils::Crypto::CreateAES_CTRImplementation(const CryptoBuffer& key)
{
#ifdef NO_SYMMETRIC_ENCRYPTION
    return nullptr;
#endif
    return GetAES_CTRFactory()->CreateImplementation(key);
}

std::shared_ptr<SymmetricCipher> Aws::Utils::Crypto::CreateAES_CTRImplementation(const CryptoBuffer& key, const CryptoBuffer& iv)
{
#ifdef NO_SYMMETRIC_ENCRYPTION
    return nullptr;
#endif
    return GetAES_CTRFactory()->CreateImplementation(key, iv);
}

std::shared_ptr<SymmetricCipher> Aws::Utils::Crypto::CreateAES_CTRImplementation(CryptoBuffer&& key, CryptoBuffer&& iv)
{
#ifdef NO_SYMMETRIC_ENCRYPTION
    return nullptr;
#endif
    return GetAES_CTRFactory()->CreateImplementation(std::move(key), std::move(iv));
}

std::shared_ptr<SymmetricCipher> Aws::Utils::Crypto::CreateAES_GCMImplementation(const CryptoBuffer& key)
{
#ifdef NO_SYMMETRIC_ENCRYPTION
    return nullptr;
#endif
    return GetAES_GCMFactory()->CreateImplementation(key);
}

std::shared_ptr<SymmetricCipher> Aws::Utils::Crypto::CreateAES_GCMImplementation(const CryptoBuffer& key, const CryptoBuffer* aad)
{
#ifdef NO_SYMMETRIC_ENCRYPTION
    return nullptr;
#endif
    return GetAES_GCMFactory()->CreateImplementation(key, aad);
}

std::shared_ptr<SymmetricCipher> Aws::Utils::Crypto::CreateAES_GCMImplementation(const CryptoBuffer& key, const CryptoBuffer& iv, const CryptoBuffer& tag, const CryptoBuffer& aad)
{
#ifdef NO_SYMMETRIC_ENCRYPTION
    return nullptr;
#endif
    return GetAES_GCMFactory()->CreateImplementation(key, iv, tag, aad);
}

std::shared_ptr<SymmetricCipher> Aws::Utils::Crypto::CreateAES_GCMImplementation(CryptoBuffer&& key, CryptoBuffer&& iv, CryptoBuffer&& tag, CryptoBuffer&& aad)
{
#ifdef NO_SYMMETRIC_ENCRYPTION
    return nullptr;
#endif
    return GetAES_GCMFactory()->CreateImplementation(std::move(key), std::move(iv), std::move(tag), std::move(aad));
}

std::shared_ptr<SymmetricCipher> Aws::Utils::Crypto::CreateAES_KeyWrapImplementation(const CryptoBuffer& key)
{
#ifdef NO_SYMMETRIC_ENCRYPTION
    return nullptr;
#endif
    return GetAES_KeyWrapFactory()->CreateImplementation(key);
}

#ifdef _WIN32
#pragma warning(pop)
#endif

std::shared_ptr<SecureRandomBytes> Aws::Utils::Crypto::CreateSecureRandomBytesImplementation()
{
    return GetSecureRandomFactory()->CreateImplementation();
}
