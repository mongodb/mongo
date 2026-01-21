// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "azure/core/http/policies/policy.hpp"

#include "azure/core/http/http.hpp"

using Azure::Core::Context;
using namespace Azure::Core::Http;
using namespace Azure::Core::Http::Policies;

// The NextHttpPolicy can't be created from a nullptr because it is a reference. So we don't need to
// check if m_policies is nullptr.
std::unique_ptr<RawResponse> NextHttpPolicy::Send(Request& request, Context const& context)
{
  if (m_index == m_policies.size() - 1)
  {
    // All the policies have run without running a transport policy
    throw std::invalid_argument("Invalid pipeline. No transport policy found. Endless policy.");
  }

  return m_policies[m_index + 1]->Send(request, NextHttpPolicy{m_index + 1, m_policies}, context);
}
