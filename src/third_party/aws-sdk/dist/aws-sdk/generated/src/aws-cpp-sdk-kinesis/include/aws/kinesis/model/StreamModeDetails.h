/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/kinesis/Kinesis_EXPORTS.h>
#include <aws/kinesis/model/StreamMode.h>
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
namespace Kinesis
{
namespace Model
{

  /**
   * <p> Specifies the capacity mode to which you want to set your data stream.
   * Currently, in Kinesis Data Streams, you can choose between an <b>on-demand</b>
   * capacity mode and a <b>provisioned</b> capacity mode for your data streams.
   * </p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/kinesis-2013-12-02/StreamModeDetails">AWS
   * API Reference</a></p>
   */
  class StreamModeDetails
  {
  public:
    AWS_KINESIS_API StreamModeDetails();
    AWS_KINESIS_API StreamModeDetails(Aws::Utils::Json::JsonView jsonValue);
    AWS_KINESIS_API StreamModeDetails& operator=(Aws::Utils::Json::JsonView jsonValue);
    AWS_KINESIS_API Aws::Utils::Json::JsonValue Jsonize() const;


    ///@{
    /**
     * <p> Specifies the capacity mode to which you want to set your data stream.
     * Currently, in Kinesis Data Streams, you can choose between an <b>on-demand</b>
     * capacity mode and a <b>provisioned</b> capacity mode for your data streams. </p>
     */
    inline const StreamMode& GetStreamMode() const{ return m_streamMode; }
    inline bool StreamModeHasBeenSet() const { return m_streamModeHasBeenSet; }
    inline void SetStreamMode(const StreamMode& value) { m_streamModeHasBeenSet = true; m_streamMode = value; }
    inline void SetStreamMode(StreamMode&& value) { m_streamModeHasBeenSet = true; m_streamMode = std::move(value); }
    inline StreamModeDetails& WithStreamMode(const StreamMode& value) { SetStreamMode(value); return *this;}
    inline StreamModeDetails& WithStreamMode(StreamMode&& value) { SetStreamMode(std::move(value)); return *this;}
    ///@}
  private:

    StreamMode m_streamMode;
    bool m_streamModeHasBeenSet = false;
  };

} // namespace Model
} // namespace Kinesis
} // namespace Aws
