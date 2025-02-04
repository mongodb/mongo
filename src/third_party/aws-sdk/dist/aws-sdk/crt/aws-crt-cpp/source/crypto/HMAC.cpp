/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/crt/crypto/HMAC.h>

#include <aws/cal/hmac.h>

namespace Aws
{
    namespace Crt
    {
        namespace Crypto
        {
            bool ComputeSHA256HMAC(
                Allocator *allocator,
                const ByteCursor &secret,
                const ByteCursor &input,
                ByteBuf &output,
                size_t truncateTo) noexcept
            {
                auto hmac = HMAC::CreateSHA256HMAC(allocator, secret);
                if (hmac)
                {
                    return hmac.ComputeOneShot(input, output, truncateTo);
                }

                return false;
            }

            bool ComputeSHA256HMAC(
                const ByteCursor &secret,
                const ByteCursor &input,
                ByteBuf &output,
                size_t truncateTo) noexcept
            {
                return ComputeSHA256HMAC(ApiAllocator(), secret, input, output, truncateTo);
            }

            HMAC::HMAC(aws_hmac *hmac) noexcept : m_hmac(hmac), m_good(false), m_lastError(0)
            {
                if (hmac)
                {
                    m_good = true;
                }
                else
                {
                    m_lastError = aws_last_error();
                }
            }

            HMAC::~HMAC()
            {
                if (m_hmac)
                {
                    aws_hmac_destroy(m_hmac);
                    m_hmac = nullptr;
                }
            }

            HMAC::HMAC(HMAC &&toMove) : m_hmac(toMove.m_hmac), m_good(toMove.m_good), m_lastError(toMove.m_lastError)
            {
                toMove.m_hmac = nullptr;
                toMove.m_good = false;
            }

            HMAC &HMAC::operator=(HMAC &&toMove)
            {
                if (&toMove != this)
                {
                    *this = HMAC(std::move(toMove));
                }

                return *this;
            }

            HMAC HMAC::CreateSHA256HMAC(Allocator *allocator, const ByteCursor &secret) noexcept
            {
                return HMAC(aws_sha256_hmac_new(allocator, &secret));
            }

            HMAC HMAC::CreateSHA256HMAC(const ByteCursor &secret) noexcept
            {
                return HMAC(aws_sha256_hmac_new(ApiAllocator(), &secret));
            }

            bool HMAC::Update(const ByteCursor &toHMAC) noexcept
            {
                if (!*this)
                {
                    return false;
                }

                if (AWS_OP_SUCCESS != aws_hmac_update(m_hmac, &toHMAC))
                {
                    m_lastError = aws_last_error();
                    m_good = false;
                    return false;
                }
                return true;
            }

            bool HMAC::Digest(ByteBuf &output, size_t truncateTo) noexcept
            {
                if (!*this)
                {
                    return false;
                }

                m_good = false;
                if (AWS_OP_SUCCESS != aws_hmac_finalize(m_hmac, &output, truncateTo))
                {
                    m_lastError = aws_last_error();
                    return false;
                }
                return true;
            }

            bool HMAC::ComputeOneShot(const ByteCursor &input, ByteBuf &output, size_t truncateTo) noexcept
            {
                if (!*this || !Update(input))
                {
                    return false;
                }

                return Digest(output, truncateTo);
            }

            size_t HMAC::DigestSize() const noexcept
            {
                if (!*this)
                {
                    return 0;
                }

                return m_hmac->digest_size;
            }

            aws_hmac_vtable ByoHMAC::s_Vtable = {
                "aws-crt-cpp-byo-crypto-hmac",
                "aws-crt-cpp-byo-crypto",
                ByoHMAC::s_Destroy,
                ByoHMAC::s_Update,
                ByoHMAC::s_Finalize,
            };

            ByoHMAC::ByoHMAC(size_t digestSize, const ByteCursor &, Allocator *allocator)
            {
                AWS_ZERO_STRUCT(m_hmacValue);
                m_hmacValue.impl = reinterpret_cast<void *>(this);
                m_hmacValue.digest_size = digestSize;
                m_hmacValue.allocator = allocator;
                m_hmacValue.good = true;
                m_hmacValue.vtable = &s_Vtable;
            }

            aws_hmac *ByoHMAC::SeatForCInterop(const std::shared_ptr<ByoHMAC> &selfRef)
            {
                AWS_FATAL_ASSERT(this == selfRef.get());
                m_selfReference = selfRef;
                return &m_hmacValue;
            }

            void ByoHMAC::s_Destroy(struct aws_hmac *hmac)
            {
                auto *byoHash = reinterpret_cast<ByoHMAC *>(hmac->impl);
                byoHash->m_selfReference = nullptr;
            }

            int ByoHMAC::s_Update(struct aws_hmac *hmac, const struct aws_byte_cursor *buf)
            {
                auto *byoHmac = reinterpret_cast<ByoHMAC *>(hmac->impl);
                if (!byoHmac->m_hmacValue.good)
                {
                    return aws_raise_error(AWS_ERROR_INVALID_STATE);
                }
                if (!byoHmac->UpdateInternal(*buf))
                {
                    byoHmac->m_hmacValue.good = false;
                    return AWS_OP_ERR;
                }
                return AWS_OP_SUCCESS;
            }

            int ByoHMAC::s_Finalize(struct aws_hmac *hmac, struct aws_byte_buf *out)
            {
                auto *byoHmac = reinterpret_cast<ByoHMAC *>(hmac->impl);
                if (!byoHmac->m_hmacValue.good)
                {
                    return aws_raise_error(AWS_ERROR_INVALID_STATE);
                }

                bool success = byoHmac->DigestInternal(*out);
                byoHmac->m_hmacValue.good = false;
                return success ? AWS_OP_SUCCESS : AWS_OP_ERR;
            }
        } // namespace Crypto
    } // namespace Crt
} // namespace Aws
