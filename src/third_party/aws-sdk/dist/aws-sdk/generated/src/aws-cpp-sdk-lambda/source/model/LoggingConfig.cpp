/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/LoggingConfig.h>
#include <aws/core/utils/json/JsonSerializer.h>

#include <utility>

using namespace Aws::Utils::Json;
using namespace Aws::Utils;

namespace Aws
{
namespace Lambda
{
namespace Model
{

LoggingConfig::LoggingConfig() : 
    m_logFormat(LogFormat::NOT_SET),
    m_logFormatHasBeenSet(false),
    m_applicationLogLevel(ApplicationLogLevel::NOT_SET),
    m_applicationLogLevelHasBeenSet(false),
    m_systemLogLevel(SystemLogLevel::NOT_SET),
    m_systemLogLevelHasBeenSet(false),
    m_logGroupHasBeenSet(false)
{
}

LoggingConfig::LoggingConfig(JsonView jsonValue)
  : LoggingConfig()
{
  *this = jsonValue;
}

LoggingConfig& LoggingConfig::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("LogFormat"))
  {
    m_logFormat = LogFormatMapper::GetLogFormatForName(jsonValue.GetString("LogFormat"));

    m_logFormatHasBeenSet = true;
  }

  if(jsonValue.ValueExists("ApplicationLogLevel"))
  {
    m_applicationLogLevel = ApplicationLogLevelMapper::GetApplicationLogLevelForName(jsonValue.GetString("ApplicationLogLevel"));

    m_applicationLogLevelHasBeenSet = true;
  }

  if(jsonValue.ValueExists("SystemLogLevel"))
  {
    m_systemLogLevel = SystemLogLevelMapper::GetSystemLogLevelForName(jsonValue.GetString("SystemLogLevel"));

    m_systemLogLevelHasBeenSet = true;
  }

  if(jsonValue.ValueExists("LogGroup"))
  {
    m_logGroup = jsonValue.GetString("LogGroup");

    m_logGroupHasBeenSet = true;
  }

  return *this;
}

JsonValue LoggingConfig::Jsonize() const
{
  JsonValue payload;

  if(m_logFormatHasBeenSet)
  {
   payload.WithString("LogFormat", LogFormatMapper::GetNameForLogFormat(m_logFormat));
  }

  if(m_applicationLogLevelHasBeenSet)
  {
   payload.WithString("ApplicationLogLevel", ApplicationLogLevelMapper::GetNameForApplicationLogLevel(m_applicationLogLevel));
  }

  if(m_systemLogLevelHasBeenSet)
  {
   payload.WithString("SystemLogLevel", SystemLogLevelMapper::GetNameForSystemLogLevel(m_systemLogLevel));
  }

  if(m_logGroupHasBeenSet)
  {
   payload.WithString("LogGroup", m_logGroup);

  }

  return payload;
}

} // namespace Model
} // namespace Lambda
} // namespace Aws
