// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "azure/core/context.hpp"

using namespace Azure::Core;

// Disable deprecation warning
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

const Context Context::ApplicationContext;

#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif // _MSC_VER

Azure::DateTime Azure::Core::Context::GetDeadline() const
{
  // Contexts form a tree. Here, we walk from a node all the way back to the root in order to find
  // the earliest deadline value.
  auto result = (DateTime::max)();
  for (std::shared_ptr<ContextSharedState> ptr = m_contextSharedState; ptr; ptr = ptr->Parent)
  {
    auto deadline = ContextSharedState::FromDateTimeRepresentation(ptr->Deadline);
    if (result > deadline)
    {
      result = deadline;
    }
  }

  return result;
}
