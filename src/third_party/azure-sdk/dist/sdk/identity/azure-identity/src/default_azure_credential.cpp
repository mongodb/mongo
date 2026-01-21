// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "azure/identity/default_azure_credential.hpp"

#include "azure/identity/azure_cli_credential.hpp"
#include "azure/identity/environment_credential.hpp"
#include "azure/identity/managed_identity_credential.hpp"
#include "azure/identity/workload_identity_credential.hpp"
#include "private/chained_token_credential_impl.hpp"
#include "private/identity_log.hpp"

using namespace Azure::Identity;
using namespace Azure::Core::Credentials;

using Azure::Core::Context;
using Azure::Core::Diagnostics::Logger;
using Azure::Identity::_detail::IdentityLog;

DefaultAzureCredential::DefaultAzureCredential(
    Core::Credentials::TokenCredentialOptions const& options)
    : TokenCredential("DefaultAzureCredential")
{
  // Initializing m_credential below and not in the member initializer list to have a specific order
  // of log messages.

  IdentityLog::Write(
      IdentityLog::Level::Verbose,
      "Creating " + GetCredentialName()
          + " which combines mutiple parameterless credentials into a single one.\n"
          + GetCredentialName()
          + " is only recommended for the early stages of development, "
            "and not for usage in production environment."
            "\nOnce the developer focuses on the Credentials and Authentication aspects "
            "of their application, "
          + GetCredentialName()
          + " needs to be replaced with the credential that "
            "is the better fit for the application.");

  // Creating credentials in order to ensure the order of log messages.
  auto const envCred = std::make_shared<EnvironmentCredential>(options);
  auto const wiCred = std::make_shared<WorkloadIdentityCredential>(options);
  auto const azCliCred = std::make_shared<AzureCliCredential>(options);
  auto const managedIdentityCred = std::make_shared<ManagedIdentityCredential>(options);

  // DefaultAzureCredential caches the selected credential, so that it can be reused on subsequent
  // calls.
  m_impl = std::make_unique<_detail::ChainedTokenCredentialImpl>(
      GetCredentialName(),
      ChainedTokenCredential::Sources{envCred, wiCred, azCliCred, managedIdentityCred},
      true);
}

DefaultAzureCredential::~DefaultAzureCredential() = default;

AccessToken DefaultAzureCredential::GetToken(
    TokenRequestContext const& tokenRequestContext,
    Context const& context) const
{
  try
  {
    return m_impl->GetToken(GetCredentialName(), tokenRequestContext, context);
  }
  catch (AuthenticationException const&)
  {
    throw AuthenticationException(
        "Failed to get token from " + GetCredentialName()
        + ".\nSee Azure::Core::Diagnostics::Logger for details "
          "(https://aka.ms/azsdk/cpp/identity/troubleshooting).");
  }
}
