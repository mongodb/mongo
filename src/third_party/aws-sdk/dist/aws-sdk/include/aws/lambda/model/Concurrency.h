/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <utility>

namespace Aws
{
namespace Utils
{
namespace Json
{
  class JsonValue;
  class JsonView;
} // namespace Json
} // namespace Utils
namespace Lambda
{
namespace Model
{

  class Concurrency
  {
  public:
    AWS_LAMBDA_API Concurrency();
    AWS_LAMBDA_API Concurrency(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API Concurrency& operator=(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API Aws::Utils::Json::JsonValue Jsonize() const;


    ///@{
    /**
     * <p>The number of concurrent executions that are reserved for this function. For
     * more information, see <a
     * href="https://docs.aws.amazon.com/lambda/latest/dg/configuration-concurrency.html">Managing
     * Lambda reserved concurrency</a>.</p>
     */
    inline int GetReservedConcurrentExecutions() const{ return m_reservedConcurrentExecutions; }
    inline bool ReservedConcurrentExecutionsHasBeenSet() const { return m_reservedConcurrentExecutionsHasBeenSet; }
    inline void SetReservedConcurrentExecutions(int value) { m_reservedConcurrentExecutionsHasBeenSet = true; m_reservedConcurrentExecutions = value; }
    inline Concurrency& WithReservedConcurrentExecutions(int value) { SetReservedConcurrentExecutions(value); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline bool RequestIdHasBeenSet() const { return m_requestIdHasBeenSet; }
    inline void SetRequestId(const Aws::String& value) { m_requestIdHasBeenSet = true; m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestIdHasBeenSet = true; m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestIdHasBeenSet = true; m_requestId.assign(value); }
    inline Concurrency& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline Concurrency& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline Concurrency& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    int m_reservedConcurrentExecutions;
    bool m_reservedConcurrentExecutionsHasBeenSet = false;

    Aws::String m_requestId;
    bool m_requestIdHasBeenSet = false;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
