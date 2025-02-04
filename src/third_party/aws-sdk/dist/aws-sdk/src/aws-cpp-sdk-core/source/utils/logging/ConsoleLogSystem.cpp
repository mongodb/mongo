/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */


#include <aws/core/utils/logging/ConsoleLogSystem.h>

#include <iostream>

using namespace Aws::Utils;
using namespace Aws::Utils::Logging;

void ConsoleLogSystem::ProcessFormattedStatement(Aws::String&& statement)
{
    std::cout << statement;
}

void ConsoleLogSystem::Flush()
{
    std::cout.flush();
}
