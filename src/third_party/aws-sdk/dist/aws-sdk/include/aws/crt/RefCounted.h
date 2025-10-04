#pragma once
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/assert.h>
#include <memory>
#include <mutex>

namespace Aws
{
    namespace Crt
    {
        /**
         * Inherit from RefCounted to allow reference-counting from C code,
         * which will keep your C++ object alive as long as the count is non-zero.
         *
         * A class must inherit from RefCounted and std::enable_shared_from_this.
         * Your class must always be placed inside a shared_ptr (do not create on
         * the stack, or keep on the heap as a raw pointer).
         *
         * Whenever the reference count goes from 0 to 1 a shared_ptr is created
         * internally to keep this object alive. Whenever the reference count
         * goes from 1 to 0 the internal shared_ptr is reset, allowing this object
         * to be destroyed.
         */
        template <class T> class RefCounted
        {
          protected:
            RefCounted() {}
            ~RefCounted() {}

            void AcquireRef()
            {
                m_mutex.lock();
                if (m_count++ == 0)
                {
                    m_strongPtr = static_cast<T *>(this)->shared_from_this();
                }
                m_mutex.unlock();
            }

            void ReleaseRef()
            {
                // Move contents of m_strongPtr to a temp so that this
                // object can't be destroyed until the function exits.
                std::shared_ptr<T> tmpStrongPtr;

                m_mutex.lock();
                AWS_ASSERT(m_count > 0 && "refcount has gone negative");
                if (m_count-- == 1)
                {
                    std::swap(m_strongPtr, tmpStrongPtr);
                }
                m_mutex.unlock();
            }

          private:
            RefCounted(const RefCounted &) = delete;
            RefCounted &operator=(const RefCounted &) = delete;

            size_t m_count = 0;
            std::shared_ptr<T> m_strongPtr;
            std::mutex m_mutex;
        };
    } // namespace Crt
} // namespace Aws
