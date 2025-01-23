#pragma once
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/crt/Exports.h>
#include <aws/crt/Types.h>

#include <aws/cal/hash.h>

struct aws_hash;
namespace Aws
{
    namespace Crt
    {
        namespace Crypto
        {
            static const size_t SHA1_DIGEST_SIZE = AWS_SHA1_LEN;
            static const size_t SHA256_DIGEST_SIZE = AWS_SHA256_LEN;
            static const size_t MD5_DIGEST_SIZE = AWS_MD5_LEN;

            /**
             * Computes a SHA256 Hash over input, and writes the digest to output. If truncateTo is non-zero, the digest
             * will be truncated to the value of truncateTo. Returns true on success. If this function fails,
             * Aws::Crt::LastError() will contain the error that occurred. Unless you're using 'truncateTo', output
             * should have a minimum capacity of SHA256_DIGEST_SIZE.
             */
            bool AWS_CRT_CPP_API ComputeSHA256(
                Allocator *allocator,
                const ByteCursor &input,
                ByteBuf &output,
                size_t truncateTo = 0) noexcept;

            /**
             * Computes a SHA256 Hash using the default allocator over input, and writes the digest to output. If
             * truncateTo is non-zero, the digest will be truncated to the value of truncateTo. Returns true on success.
             * If this function fails, Aws::Crt::LastError() will contain the error that occurred. Unless you're using
             * 'truncateTo', output should have a minimum capacity of SHA256_DIGEST_SIZE.
             */
            bool AWS_CRT_CPP_API
                ComputeSHA256(const ByteCursor &input, ByteBuf &output, size_t truncateTo = 0) noexcept;

            /**
             * Computes a MD5 Hash over input, and writes the digest to output. If truncateTo is non-zero, the digest
             * will be truncated to the value of truncateTo. Returns true on success. If this function fails,
             * Aws::Crt::LastError() will contain the error that occurred. Unless you're using 'truncateTo',
             * output should have a minimum capacity of MD5_DIGEST_SIZE.
             */
            bool AWS_CRT_CPP_API ComputeMD5(
                Allocator *allocator,
                const ByteCursor &input,
                ByteBuf &output,
                size_t truncateTo = 0) noexcept;

            /**
             * Computes a MD5 Hash using the default allocator over input, and writes the digest to output. If
             * truncateTo is non-zero, the digest will be truncated to the value of truncateTo. Returns true on success.
             * If this function fails, Aws::Crt::LastError() will contain the error that occurred. Unless you're using
             * 'truncateTo', output should have a minimum capacity of MD5_DIGEST_SIZE.
             */
            bool AWS_CRT_CPP_API ComputeMD5(const ByteCursor &input, ByteBuf &output, size_t truncateTo = 0) noexcept;

            /**
             * Computes a SHA1 Hash over input, and writes the digest to output. If truncateTo is non-zero, the digest
             * will be truncated to the value of truncateTo. Returns true on success. If this function fails,
             * Aws::Crt::LastError() will contain the error that occurred. Unless you're using 'truncateTo',
             * output should have a minimum capacity of MD5_DIGEST_SIZE.
             */
            bool AWS_CRT_CPP_API ComputeSHA1(
                Allocator *allocator,
                const ByteCursor &input,
                ByteBuf &output,
                size_t truncateTo = 0) noexcept;

            /**
             * Computes a SHA1 Hash using the default allocator over input, and writes the digest to output. If
             * truncateTo is non-zero, the digest will be truncated to the value of truncateTo. Returns true on success.
             * If this function fails, Aws::Crt::LastError() will contain the error that occurred. Unless you're using
             * 'truncateTo', output should have a minimum capacity of SHA1_DIGEST_SIZE.
             */
            bool AWS_CRT_CPP_API ComputeSHA1(const ByteCursor &input, ByteBuf &output, size_t truncateTo = 0) noexcept;

            /**
             * Streaming Hash object. The typical use case is for computing the hash of an object that is too large to
             * load into memory. You can call Update() multiple times as you load chunks of data into memory. When
             * you're finished simply call Digest(). After Digest() is called, this object is no longer usable.
             */
            class AWS_CRT_CPP_API Hash final
            {
              public:
                ~Hash();
                Hash(const Hash &) = delete;
                Hash &operator=(const Hash &) = delete;
                Hash(Hash &&toMove);
                Hash &operator=(Hash &&toMove);

                /**
                 * Returns true if the instance is in a valid state, false otherwise.
                 */
                operator bool() const noexcept;

                /**
                 * Returns the value of the last aws error encountered by operations on this instance.
                 */
                inline int LastError() const noexcept { return m_lastError; }

                /**
                 * Creates an instance of a Streaming SHA256 Hash.
                 */
                static Hash CreateSHA256(Allocator *allocator = ApiAllocator()) noexcept;

                /**
                 * Creates an instance of a Stream SHA1 Hash.
                 */
                static Hash CreateSHA1(Allocator *allocator = ApiAllocator()) noexcept;

                /**
                 * Creates an instance of a Streaming MD5 Hash.
                 */
                static Hash CreateMD5(Allocator *allocator = ApiAllocator()) noexcept;

                /**
                 * Updates the running hash object with data in toHash. Returns true on success. Call
                 * LastError() for the reason this call failed.
                 */
                bool Update(const ByteCursor &toHash) noexcept;

                /**
                 * Finishes the running hash operation and writes the digest into output. The available capacity of
                 * output must be large enough for the digest. See: SHA1_DIGEST_SIZE, SHA256_DIGEST_SIZE and
                 * MD5_DIGEST_SIZE for size hints. 'truncateTo' is for if you want truncated output (e.g. you only want
                 * the first 16 bytes of a SHA256 digest. Returns true on success. Call LastError() for the reason this
                 * call failed.
                 */
                bool Digest(ByteBuf &output, size_t truncateTo = 0) noexcept;

                /**
                 * Computes the hash of input and writes the digest into output. The available capacity of
                 * output must be large enough for the digest. See: SHA1_DIGEST_SIZE, SHA256_DIGEST_SIZE and
                 * MD5_DIGEST_SIZE for size hints. 'truncateTo' is for if you want truncated output (e.g. you only want
                 * the first 16 bytes of a SHA256 digest. Returns true on success. Call LastError() for the reason this
                 * call failed.
                 *
                 * This is an API a user would use for smaller size inputs. For larger, streaming inputs, use
                 * multiple calls to Update() for each buffer, followed by a single call to Digest().
                 */
                bool ComputeOneShot(const ByteCursor &input, ByteBuf &output, size_t truncateTo = 0) noexcept;

                /**
                 * Returns the size of the digest for this hash algorithm. If this object is not valid, it will
                 * return 0 instead.
                 */
                size_t DigestSize() const noexcept;

              private:
                Hash(aws_hash *hash) noexcept;
                Hash() = delete;

                aws_hash *m_hash;
                int m_lastError;
            };

            /**
             * BYO_CRYPTO: Base class for custom hash implementations.
             *
             * If using BYO_CRYPTO, you must define concrete implementations for the required hash algorithms
             * and set their creation callbacks via functions like ApiHandle.SetBYOCryptoNewMD5Callback().
             */
            class AWS_CRT_CPP_API ByoHash
            {
              public:
                virtual ~ByoHash();

                /** @private
                 * this is called by the framework. If you're trying to create instances of this class manually,
                 * please don't. But if you do. Look at the other factory functions for reference.
                 */
                aws_hash *SeatForCInterop(const std::shared_ptr<ByoHash> &selfRef);

              protected:
                ByoHash(size_t digestSize, Allocator *allocator = ApiAllocator());

                /**
                 * Update the running hash with to_hash.
                 * This can be called multiple times.
                 * Raise an AWS error and return false to indicate failure.
                 */
                virtual bool UpdateInternal(const ByteCursor &toHash) noexcept = 0;

                /**
                 * Complete the hash computation and write the final digest to output.
                 * This cannot be called more than once.
                 * If truncate_to is something other than 0, the output must be truncated to that number of bytes.
                 * Raise an AWS error and return false to indicate failure.
                 */
                virtual bool DigestInternal(ByteBuf &output, size_t truncateTo = 0) noexcept = 0;

              private:
                static void s_Destroy(struct aws_hash *hash);
                static int s_Update(struct aws_hash *hash, const struct aws_byte_cursor *buf);
                static int s_Finalize(struct aws_hash *hash, struct aws_byte_buf *out);

                static aws_hash_vtable s_Vtable;
                aws_hash m_hashValue;
                std::shared_ptr<ByoHash> m_selfReference;
            };

            using CreateHashCallback = std::function<std::shared_ptr<ByoHash>(size_t digestSize, Allocator *)>;

        } // namespace Crypto
    } // namespace Crt
} // namespace Aws
