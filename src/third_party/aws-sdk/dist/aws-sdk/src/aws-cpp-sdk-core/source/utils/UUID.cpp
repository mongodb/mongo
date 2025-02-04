/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/utils/UUID.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/crypto/Factories.h>
#include <aws/core/utils/crypto/SecureRandom.h>
#include <iomanip>
#include <random>
#include <chrono>
#include <thread>
#include <mutex>

namespace Aws
{
    namespace Utils
    {
        static const size_t UUID_STR_SIZE = 0x24u; // 36 characters
        static const size_t VERSION_LOCATION = 0x06u;
        static const size_t VARIANT_LOCATION = 0x08u;
        static const unsigned char VERSION = 0x40u;
        static const unsigned char VERSION_MASK = 0x0Fu;
        static const unsigned char VARIANT = 0x80u;
        static const unsigned char VARIANT_MASK = 0x3Fu;

        static void hexify(Aws::String& ss, const unsigned char* toWrite, size_t min, size_t max)
        {
            for (size_t i = min; i < max; ++i)
            {
                ss.push_back("0123456789ABCDEF"[toWrite[i] >> 4]);
                ss.push_back("0123456789ABCDEF"[toWrite[i] & 0x0F]);
            }
        }

        Aws::Utils::UUID::UUID(const Aws::String& uuidToConvert)
        {
            //GUID has 2 characters per byte + 4 dashes = 36 bytes
            assert(uuidToConvert.length() == UUID_STR_SIZE);
            memset(m_uuid, 0, sizeof(m_uuid));
            Aws::String escapedHexStr(uuidToConvert);
            StringUtils::Replace(escapedHexStr, "-", "");
            assert(escapedHexStr.length() == UUID_BINARY_SIZE * 2);
            ByteBuffer&& rawUuid = HashingUtils::HexDecode(escapedHexStr);
            memcpy(m_uuid, rawUuid.GetUnderlyingData(), rawUuid.GetLength());
        }

        Aws::Utils::UUID::UUID(const unsigned char toCopy[UUID_BINARY_SIZE])
        {
            memcpy(m_uuid, toCopy, sizeof(m_uuid));
        }

        Aws::Utils::UUID::operator Aws::String() const
        {
            Aws::String ss;
            ss.reserve(UUID_STR_SIZE);
            hexify(ss, m_uuid, 0, 4);
            ss.push_back('-');

            hexify(ss, m_uuid, 4, 6);
            ss.push_back('-');

            hexify(ss, m_uuid, 6, 8);
            ss.push_back('-');

            hexify(ss, m_uuid, 8, 10);
            ss.push_back('-');

            hexify(ss, m_uuid, 10, 16);

            return ss;
        }

        Aws::Utils::UUID UUID::RandomUUID()
        {
            auto secureRandom = Crypto::CreateSecureRandomBytesImplementation();
            assert(secureRandom);

            unsigned char randomBytes[UUID_BINARY_SIZE];
            memset(randomBytes, 0, UUID_BINARY_SIZE);
            secureRandom->GetBytes(randomBytes, UUID_BINARY_SIZE);
            //Set version bits to 0100
            //https://tools.ietf.org/html/rfc4122#section-4.1.3
            randomBytes[VERSION_LOCATION] = (randomBytes[VERSION_LOCATION] & VERSION_MASK) | VERSION;
            //set variant bits to 10
            //https://tools.ietf.org/html/rfc4122#section-4.1.1
            randomBytes[VARIANT_LOCATION] = (randomBytes[VARIANT_LOCATION] & VARIANT_MASK) | VARIANT;

            return Aws::Utils::UUID(randomBytes);
        }

#ifdef UINT64_MAX
        using MTEngine = std::mt19937_64;
        using RandGenType = uint64_t;
#else
        using MTEngine = std::mt19937;
        using RandGenType = unsigned int;
#endif

        static size_t GetCurrentThreadRandomSeed()
        {
            static size_t processRandomSeed = std::random_device{}();
            static MTEngine threadRandomSeedGen(processRandomSeed);
            // Threads can be re-used (esp. on OS X), generate a true random per-thread random seed
            static std::mutex threadRandomSeedGenMtx;
            std::unique_lock<std::mutex> lock(threadRandomSeedGenMtx);
            return static_cast<size_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()) ^ threadRandomSeedGen());
        }

        Aws::Utils::UUID UUID::PseudoRandomUUID()
        {
            static const thread_local size_t threadSeed = GetCurrentThreadRandomSeed();
            static thread_local MTEngine gen(threadSeed);

            unsigned char randomBytes[UUID_BINARY_SIZE] = {0};

            for (size_t i = 0; i < UUID_BINARY_SIZE / sizeof(RandGenType); i++) {
                reinterpret_cast<RandGenType*>(randomBytes)[i] = gen();
            }

            //Set version bits to 0100
            //https://tools.ietf.org/html/rfc4122#section-4.1.3
            randomBytes[VERSION_LOCATION] = (randomBytes[VERSION_LOCATION] & VERSION_MASK) | VERSION;
            //set variant bits to 10
            //https://tools.ietf.org/html/rfc4122#section-4.1.1
            randomBytes[VARIANT_LOCATION] = (randomBytes[VARIANT_LOCATION] & VARIANT_MASK) | VARIANT;

            return Aws::Utils::UUID(randomBytes);
        }
    }
}
