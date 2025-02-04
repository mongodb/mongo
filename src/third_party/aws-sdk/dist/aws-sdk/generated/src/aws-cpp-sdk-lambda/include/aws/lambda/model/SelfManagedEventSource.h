/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSMap.h>
#include <aws/lambda/model/EndPointType.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
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

  /**
   * <p>The self-managed Apache Kafka cluster for your event source.</p><p><h3>See
   * Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/SelfManagedEventSource">AWS
   * API Reference</a></p>
   */
  class SelfManagedEventSource
  {
  public:
    AWS_LAMBDA_API SelfManagedEventSource();
    AWS_LAMBDA_API SelfManagedEventSource(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API SelfManagedEventSource& operator=(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API Aws::Utils::Json::JsonValue Jsonize() const;


    ///@{
    /**
     * <p>The list of bootstrap servers for your Kafka brokers in the following format:
     * <code>"KAFKA_BOOTSTRAP_SERVERS":
     * ["abc.xyz.com:xxxx","abc2.xyz.com:xxxx"]</code>.</p>
     */
    inline const Aws::Map<EndPointType, Aws::Vector<Aws::String>>& GetEndpoints() const{ return m_endpoints; }
    inline bool EndpointsHasBeenSet() const { return m_endpointsHasBeenSet; }
    inline void SetEndpoints(const Aws::Map<EndPointType, Aws::Vector<Aws::String>>& value) { m_endpointsHasBeenSet = true; m_endpoints = value; }
    inline void SetEndpoints(Aws::Map<EndPointType, Aws::Vector<Aws::String>>&& value) { m_endpointsHasBeenSet = true; m_endpoints = std::move(value); }
    inline SelfManagedEventSource& WithEndpoints(const Aws::Map<EndPointType, Aws::Vector<Aws::String>>& value) { SetEndpoints(value); return *this;}
    inline SelfManagedEventSource& WithEndpoints(Aws::Map<EndPointType, Aws::Vector<Aws::String>>&& value) { SetEndpoints(std::move(value)); return *this;}
    inline SelfManagedEventSource& AddEndpoints(const EndPointType& key, const Aws::Vector<Aws::String>& value) { m_endpointsHasBeenSet = true; m_endpoints.emplace(key, value); return *this; }
    inline SelfManagedEventSource& AddEndpoints(EndPointType&& key, const Aws::Vector<Aws::String>& value) { m_endpointsHasBeenSet = true; m_endpoints.emplace(std::move(key), value); return *this; }
    inline SelfManagedEventSource& AddEndpoints(const EndPointType& key, Aws::Vector<Aws::String>&& value) { m_endpointsHasBeenSet = true; m_endpoints.emplace(key, std::move(value)); return *this; }
    inline SelfManagedEventSource& AddEndpoints(EndPointType&& key, Aws::Vector<Aws::String>&& value) { m_endpointsHasBeenSet = true; m_endpoints.emplace(std::move(key), std::move(value)); return *this; }
    ///@}
  private:

    Aws::Map<EndPointType, Aws::Vector<Aws::String>> m_endpoints;
    bool m_endpointsHasBeenSet = false;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
