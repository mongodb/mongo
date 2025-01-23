/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/AmazonSerializableWebServiceRequest.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

using namespace Aws;

std::shared_ptr<Aws::IOStream> AmazonSerializableWebServiceRequest::GetBody() const
{
    Aws::String&& payload = SerializePayload();
    std::shared_ptr<Aws::IOStream> payloadBody;

    if (!payload.empty())
    {
      payloadBody = Aws::MakeShared<Aws::StringStream>("AmazonSerializableWebServiceRequest");
      *payloadBody << payload;
    }

    return payloadBody;
}

