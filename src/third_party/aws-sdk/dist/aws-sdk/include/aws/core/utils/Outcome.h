/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>

#include <cassert>
#include <utility>

namespace Aws
{
    namespace Utils
    {

        /**
         * Template class representing the outcome of making a request.  It will contain
         * either a successful result or the failure error.  The caller must check
         * whether the outcome of the request was a success before attempting to access
         *  the result or the error.
         */
        template<typename R, typename E> // Result, Error
        class Outcome
        {
        public:

            Outcome() : result(), error(), success(false)
            {
            }
            Outcome(const R& r) : result(r), error(), success(true)
            {
            }
            Outcome(const E& e) : result(), error(e), success(false)
            {
            }
            Outcome(R&& r) : result(std::forward<R>(r)), error(), success(true)
            {
            }
            Outcome(E&& e) : result(), error(std::forward<E>(e)), success(false)
            {
            }
            Outcome(const Outcome& o) :
                result(o.result),
                error(o.error),
                success(o.success)
            {
            }

            template<typename RT, typename ET>
            friend class Outcome;

#if defined (__cplusplus) && __cplusplus > 201103L
            template< bool B, class T = void >
            using enable_if_t = std::enable_if_t<B, T>;
#else
            template< bool B, class T = void >
            using enable_if_t = typename std::enable_if<B,T>::type;
#endif

            // Move both result and error from other type of outcome
            template<typename RT, typename ET, enable_if_t<std::is_convertible<RT, R>::value &&
                                                           std::is_convertible<ET, E>::value, int> = 0>
            Outcome(Outcome<RT, ET>&& o) :
                result(std::move(o.result)),
                error(std::move(o.error)),
                success(o.success)
            {
            }

            // Move result from other type of outcome
            template<typename RT, typename ET, enable_if_t<std::is_convertible<RT, R>::value &&
                                                          !std::is_convertible<ET, E>::value, int> = 0>
            Outcome(Outcome<RT, ET>&& o) :
                result(std::move(o.result)),
                success(o.success)
            {
                assert(o.success);
            }

            // Move error from other type of outcome
            template<typename RT, typename ET, enable_if_t<!std::is_convertible<RT, R>::value &&
                                                            std::is_convertible<ET, E>::value, int> = 0>
            Outcome(Outcome<RT, ET>&& o) :
                error(std::move(o.error)),
                success(o.success)
            {
                assert(!o.success);
            }

            template<typename ET, enable_if_t<std::is_convertible<ET, E>::value, int> = 0>
            Outcome(ET&& e) : error(std::forward<ET>(e)), success(false)
            {
            }

            Outcome& operator=(const Outcome& o)
            {
                if (this != &o)
                {
                    result = o.result;
                    error = o.error;
                    success = o.success;
                    retryCount = o.retryCount;
                }

                return *this;
            }

            Outcome(Outcome&& o) : // Required to force Move Constructor
                result(std::move(o.result)),
                error(std::move(o.error)),
                success(o.success),
                retryCount(std::move(o.retryCount))
            {
            }

            Outcome& operator=(Outcome&& o)
            {
                if (this != &o)
                {
                    result = std::move(o.result);
                    error = std::move(o.error);
                    success = o.success;
                    retryCount = std::move(o.retryCount);
                }

                return *this;
            }

            inline const R& GetResult() const
            {
                return result;
            }

            inline R& GetResult()
            {
                return result;
            }

            /**
             * casts the underlying result to an r-value so that caller can take ownership of underlying resources.
             * this is necessary when streams are involved.
             */
            inline R&& GetResultWithOwnership()
            {
                return std::move(result);
            }

            inline const E& GetError() const
            {
                return error;
            }

            template<typename T>
            inline T GetError()
            {
                return error.template GetModeledError<T>();
            }

            inline bool IsSuccess() const
            {
                return this->success;
            }

            /**
             * Returns how many times the retry happened before getting this outcome.
             */
            inline unsigned int GetRetryCount() const { return retryCount; }
            /**
             * Sets the retry count.
             */
            inline void SetRetryCount(const unsigned int iRetryCount) { retryCount = iRetryCount; }

        private:
            R result;
            E error;
            bool success = false;
            unsigned int retryCount = 0;
        };

    } // namespace Utils
} // namespace Aws
