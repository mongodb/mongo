/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/utils/Array.h>

#include <aws/core/platform/Security.h>

namespace Aws
{
    namespace Utils
    {
            Array<CryptoBuffer> CryptoBuffer::Slice(size_t sizeOfSlice) const
            {
                assert(sizeOfSlice <= GetLength());

                size_t numberOfSlices = (GetLength() + sizeOfSlice - 1) / sizeOfSlice;
                size_t currentSliceIndex = 0;
                Array<CryptoBuffer> slices(numberOfSlices);

                for (size_t i = 0; i < numberOfSlices - 1; ++i)
                {
                    CryptoBuffer newArray(sizeOfSlice);
                    for (size_t cpyIdx = 0; cpyIdx < newArray.GetLength(); ++cpyIdx)
                    {
                        newArray[cpyIdx] = GetItem(cpyIdx + currentSliceIndex);
                    }
                    currentSliceIndex += sizeOfSlice;
                    slices[i] = std::move(newArray);
                }

                CryptoBuffer lastArray(GetLength() % sizeOfSlice == 0 ? sizeOfSlice : GetLength() % sizeOfSlice );
                for (size_t cpyIdx = 0; cpyIdx < lastArray.GetLength(); ++cpyIdx)
                {
                    lastArray[cpyIdx] = GetItem(cpyIdx + currentSliceIndex);
                }
                slices[slices.GetLength() - 1] = std::move(lastArray);

                return slices;
            }            

            CryptoBuffer& CryptoBuffer::operator^(const CryptoBuffer& operand)
            {
                size_t smallestSize = std::min<size_t>(GetLength(), operand.GetLength());
                for (size_t i = 0; i < smallestSize; ++i)
                {
                    (*this)[i] ^= operand[i];
                }

                return *this;
            }

            /**
            * Zero out the array securely
            */
            void CryptoBuffer::Zero()
            {
                if (GetUnderlyingData())
                {
                    Aws::Security::SecureMemClear(GetUnderlyingData(), GetLength());
                }
            }
    }
}