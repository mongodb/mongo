// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief Responses of paginated collections from the service.
 */

#pragma once

#include "azure/core/context.hpp"
#include "azure/core/http/raw_response.hpp"
#include "azure/core/nullable.hpp"

#include <cstdlib>
#include <string>

namespace Azure { namespace Core {

  /**
   * @brief The base type and behavior for a paged response.
   *
   * @remark The template is used for static-inheritance.
   *
   * @remark T classes must implement the way to get and move to the next page.
   *
   * @tparam T A class type for static-inheritance.
   */
  template <class T> class PagedResponse {
  private:
    // The field used to check when the end of the response is reached. We default it true as the
    // starting point because all responses from a service will always come with a payload that
    // represents at least one page. The page might or might not contain elements in the page.
    // `m_hasPage` is then turned to `false` once `MoveToNextPage` is called on the last page.
    bool m_hasPage = true;

  protected:
    /**
     * @brief Constructs a default instance of `%PagedResponse`.
     *
     */
    PagedResponse() = default;

    /**
     * @brief Constructs `%PagedResponse` by moving in another instance.
     *
     */
    PagedResponse(PagedResponse&&) = default;

    /**
     * @brief Assigns another instance of `%PagedResponse` by moving it in.
     *
     */
    PagedResponse& operator=(PagedResponse&&) = default;

  public:
    /**
     * @brief Destructs `%PagedResponse`.
     *
     */
    virtual ~PagedResponse() = default;

    /**
     * @brief The token used to fetch the current page.
     *
     */
    std::string CurrentPageToken;

    /**
     * @brief The token for getting the next page.
     *
     * @note If there are no more pages, this field becomes an empty string.
     *
     * @note Assumes all services will include NextPageToken in the payload, it is set to either
     * null or empty for the last page or to a value used for getting the next page.
     *
     */
    Azure::Nullable<std::string> NextPageToken;

    /**
     * @brief The HTTP response returned by the service.
     *
     */
    std::unique_ptr<Azure::Core::Http::RawResponse> RawResponse;

    /**
     * @brief Checks if a page exists.
     *
     * @note Returns false after the last page.
     * @return `true` if there are additional pages; otherwise, `false`.
     *
     */
    bool HasPage() const { return m_hasPage; }

    /**
     * @brief Moves to the next page of the response.
     *
     * @note Calling this method on the last page will set #HasPage() to `false`.
     *
     * @param context A context to control the request lifetime.
     */
    void MoveToNextPage(const Azure::Core::Context& context = Azure::Core::Context())
    {
      static_assert(
          std::is_base_of<PagedResponse, T>::value,
          "The template argument \"T\" should derive from PagedResponse<T>.");

      if (!NextPageToken.HasValue() || NextPageToken.Value().empty())
      {
        m_hasPage = false;
        return;
      }

      // Developer must make sure current page is kept unchanged if OnNextPage()
      // throws exception.
      static_cast<T*>(this)->OnNextPage(context);
    }
  };

}} // namespace Azure::Core
