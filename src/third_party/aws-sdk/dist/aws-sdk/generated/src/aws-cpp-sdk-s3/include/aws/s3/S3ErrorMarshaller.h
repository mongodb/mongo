/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/client/AWSErrorMarshaller.h>

namespace Aws
{
namespace Client
{

class AWS_S3_API S3ErrorMarshaller : public Aws::Client::XmlErrorMarshaller
{
public:
  Aws::Client::AWSError<Aws::Client::CoreErrors> FindErrorByName(const char* exceptionName) const override;
  virtual Aws::String ExtractRegion(const AWSError<CoreErrors>&) const override;
  virtual Aws::String ExtractEndpoint(const AWSError<CoreErrors>&) const override;
  virtual AWSError<Aws::Client::CoreErrors>  Marshall(const Aws::Http::HttpResponse& response) const override;
};

} // namespace Client
} // namespace Aws