/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/kinesis/model/StreamModeDetails.h>
#include <aws/core/utils/json/JsonSerializer.h>

#include <utility>

using namespace Aws::Utils::Json;
using namespace Aws::Utils;

namespace Aws
{
namespace Kinesis
{
namespace Model
{

StreamModeDetails::StreamModeDetails() : 
    m_streamMode(StreamMode::NOT_SET),
    m_streamModeHasBeenSet(false)
{
}

StreamModeDetails::StreamModeDetails(JsonView jsonValue)
  : StreamModeDetails()
{
  *this = jsonValue;
}

StreamModeDetails& StreamModeDetails::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("StreamMode"))
  {
    m_streamMode = StreamModeMapper::GetStreamModeForName(jsonValue.GetString("StreamMode"));

    m_streamModeHasBeenSet = true;
  }

  return *this;
}

JsonValue StreamModeDetails::Jsonize() const
{
  JsonValue payload;

  if(m_streamModeHasBeenSet)
  {
   payload.WithString("StreamMode", StreamModeMapper::GetNameForStreamMode(m_streamMode));
  }

  return payload;
}

} // namespace Model
} // namespace Kinesis
} // namespace Aws
