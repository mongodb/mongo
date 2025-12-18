/*
 * Copyright The OpenTelemetry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * DO NOT EDIT, this is an Auto-generated file from:
 * buildscripts/semantic-convention/templates/registry/semantic_attributes-h.j2
 */

#pragma once

#include "opentelemetry/common/macros.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace semconv
{
namespace system
{

/**
  Deprecated, use @code cpu.logical_number @endcode instead.

  @deprecated
  {"note": "Replaced by @code cpu.logical_number @endcode.", "reason": "renamed", "renamed_to":
  "cpu.logical_number"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kSystemCpuLogicalNumber =
    "system.cpu.logical_number";

/**
  Deprecated, use @code cpu.mode @endcode instead.

  @deprecated
  {"note": "Replaced by @code cpu.mode @endcode.", "reason": "renamed", "renamed_to": "cpu.mode"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kSystemCpuState = "system.cpu.state";

/**
  The device identifier
 */
static constexpr const char *kSystemDevice = "system.device";

/**
  The filesystem mode
 */
static constexpr const char *kSystemFilesystemMode = "system.filesystem.mode";

/**
  The filesystem mount path
 */
static constexpr const char *kSystemFilesystemMountpoint = "system.filesystem.mountpoint";

/**
  The filesystem state
 */
static constexpr const char *kSystemFilesystemState = "system.filesystem.state";

/**
  The filesystem type
 */
static constexpr const char *kSystemFilesystemType = "system.filesystem.type";

/**
  The memory state
 */
static constexpr const char *kSystemMemoryState = "system.memory.state";

/**
  Deprecated, use @code network.connection.state @endcode instead.

  @deprecated
  {"note": "Replaced by @code network.connection.state @endcode.", "reason": "renamed",
  "renamed_to": "network.connection.state"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kSystemNetworkState = "system.network.state";

/**
  The paging access direction
 */
static constexpr const char *kSystemPagingDirection = "system.paging.direction";

/**
  The paging fault type
 */
static constexpr const char *kSystemPagingFaultType = "system.paging.fault.type";

/**
  The memory paging state
 */
static constexpr const char *kSystemPagingState = "system.paging.state";

/**
  Deprecated, use @code system.paging.fault.type @endcode instead.

  @deprecated
  {"note": "Replaced by @code system.paging.fault.type @endcode.", "reason": "renamed",
  "renamed_to": "system.paging.fault.type"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kSystemPagingType = "system.paging.type";

/**
  Deprecated, use @code process.state @endcode instead.

  @deprecated
  {"note": "Replaced by @code process.state @endcode.", "reason": "renamed", "renamed_to":
  "process.state"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kSystemProcessStatus =
    "system.process.status";

/**
  Deprecated, use @code process.state @endcode instead.

  @deprecated
  {"note": "Replaced by @code process.state @endcode.", "reason": "renamed", "renamed_to":
  "process.state"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kSystemProcessesStatus =
    "system.processes.status";

namespace SystemCpuStateValues
{

static constexpr const char *kUser = "user";

static constexpr const char *kSystem = "system";

static constexpr const char *kNice = "nice";

static constexpr const char *kIdle = "idle";

static constexpr const char *kIowait = "iowait";

static constexpr const char *kInterrupt = "interrupt";

static constexpr const char *kSteal = "steal";

}  // namespace SystemCpuStateValues

namespace SystemFilesystemStateValues
{

static constexpr const char *kUsed = "used";

static constexpr const char *kFree = "free";

static constexpr const char *kReserved = "reserved";

}  // namespace SystemFilesystemStateValues

namespace SystemFilesystemTypeValues
{

static constexpr const char *kFat32 = "fat32";

static constexpr const char *kExfat = "exfat";

static constexpr const char *kNtfs = "ntfs";

static constexpr const char *kRefs = "refs";

static constexpr const char *kHfsplus = "hfsplus";

static constexpr const char *kExt4 = "ext4";

}  // namespace SystemFilesystemTypeValues

namespace SystemMemoryStateValues
{
/**
  Actual used virtual memory in bytes.
 */
static constexpr const char *kUsed = "used";

static constexpr const char *kFree = "free";

/**
  @deprecated
  {"note": "Removed, report shared memory usage with @code metric.system.memory.shared @endcode
  metric", "reason": "uncategorized"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kShared = "shared";

static constexpr const char *kBuffers = "buffers";

static constexpr const char *kCached = "cached";

}  // namespace SystemMemoryStateValues

namespace SystemNetworkStateValues
{

static constexpr const char *kClose = "close";

static constexpr const char *kCloseWait = "close_wait";

static constexpr const char *kClosing = "closing";

static constexpr const char *kDelete = "delete";

static constexpr const char *kEstablished = "established";

static constexpr const char *kFinWait1 = "fin_wait_1";

static constexpr const char *kFinWait2 = "fin_wait_2";

static constexpr const char *kLastAck = "last_ack";

static constexpr const char *kListen = "listen";

static constexpr const char *kSynRecv = "syn_recv";

static constexpr const char *kSynSent = "syn_sent";

static constexpr const char *kTimeWait = "time_wait";

}  // namespace SystemNetworkStateValues

namespace SystemPagingDirectionValues
{

static constexpr const char *kIn = "in";

static constexpr const char *kOut = "out";

}  // namespace SystemPagingDirectionValues

namespace SystemPagingFaultTypeValues
{

static constexpr const char *kMajor = "major";

static constexpr const char *kMinor = "minor";

}  // namespace SystemPagingFaultTypeValues

namespace SystemPagingStateValues
{

static constexpr const char *kUsed = "used";

static constexpr const char *kFree = "free";

}  // namespace SystemPagingStateValues

namespace SystemPagingTypeValues
{

static constexpr const char *kMajor = "major";

static constexpr const char *kMinor = "minor";

}  // namespace SystemPagingTypeValues

namespace SystemProcessStatusValues
{

static constexpr const char *kRunning = "running";

static constexpr const char *kSleeping = "sleeping";

static constexpr const char *kStopped = "stopped";

static constexpr const char *kDefunct = "defunct";

}  // namespace SystemProcessStatusValues

namespace SystemProcessesStatusValues
{

static constexpr const char *kRunning = "running";

static constexpr const char *kSleeping = "sleeping";

static constexpr const char *kStopped = "stopped";

static constexpr const char *kDefunct = "defunct";

}  // namespace SystemProcessesStatusValues

}  // namespace system
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
