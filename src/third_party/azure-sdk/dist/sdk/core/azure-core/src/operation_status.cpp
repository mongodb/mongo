// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "azure/core/operation_status.hpp"

namespace Azure { namespace Core {

  const OperationStatus OperationStatus::NotStarted("NotStarted");
  const OperationStatus OperationStatus::Running{"Running"};
  const OperationStatus OperationStatus::Succeeded{"Succeeded"};
  const OperationStatus OperationStatus::Failed{"Failed"};
  const OperationStatus OperationStatus::Cancelled{"Cancelled"};

}} // namespace Azure::Core
