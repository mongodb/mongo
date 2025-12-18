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
namespace hw
{

/**
  Design capacity in Watts-hours or Amper-hours
 */
static constexpr const char *kHwBatteryCapacity = "hw.battery.capacity";

/**
  Battery <a href="https://schemas.dmtf.org/wbem/cim-html/2.31.0/CIM_Battery.html">chemistry</a>,
  e.g. Lithium-Ion, Nickel-Cadmium, etc.
 */
static constexpr const char *kHwBatteryChemistry = "hw.battery.chemistry";

/**
  The current state of the battery
 */
static constexpr const char *kHwBatteryState = "hw.battery.state";

/**
  BIOS version of the hardware component
 */
static constexpr const char *kHwBiosVersion = "hw.bios_version";

/**
  Driver version for the hardware component
 */
static constexpr const char *kHwDriverVersion = "hw.driver_version";

/**
  Type of the enclosure (useful for modular systems)
 */
static constexpr const char *kHwEnclosureType = "hw.enclosure.type";

/**
  Firmware version of the hardware component
 */
static constexpr const char *kHwFirmwareVersion = "hw.firmware_version";

/**
  Type of task the GPU is performing
 */
static constexpr const char *kHwGpuTask = "hw.gpu.task";

/**
  An identifier for the hardware component, unique within the monitored host
 */
static constexpr const char *kHwId = "hw.id";

/**
  Type of limit for hardware components
 */
static constexpr const char *kHwLimitType = "hw.limit_type";

/**
  RAID Level of the logical disk
 */
static constexpr const char *kHwLogicalDiskRaidLevel = "hw.logical_disk.raid_level";

/**
  State of the logical disk space usage
 */
static constexpr const char *kHwLogicalDiskState = "hw.logical_disk.state";

/**
  Type of the memory module
 */
static constexpr const char *kHwMemoryType = "hw.memory.type";

/**
  Descriptive model name of the hardware component
 */
static constexpr const char *kHwModel = "hw.model";

/**
  An easily-recognizable name for the hardware component
 */
static constexpr const char *kHwName = "hw.name";

/**
  Logical addresses of the adapter (e.g. IP address, or WWPN)
 */
static constexpr const char *kHwNetworkLogicalAddresses = "hw.network.logical_addresses";

/**
  Physical address of the adapter (e.g. MAC address, or WWNN)
 */
static constexpr const char *kHwNetworkPhysicalAddress = "hw.network.physical_address";

/**
  Unique identifier of the parent component (typically the @code hw.id @endcode attribute of the
  enclosure, or disk controller)
 */
static constexpr const char *kHwParent = "hw.parent";

/**
  <a href="https://wikipedia.org/wiki/S.M.A.R.T.">S.M.A.R.T.</a> (Self-Monitoring, Analysis, and
  Reporting Technology) attribute of the physical disk
 */
static constexpr const char *kHwPhysicalDiskSmartAttribute = "hw.physical_disk.smart_attribute";

/**
  State of the physical disk endurance utilization
 */
static constexpr const char *kHwPhysicalDiskState = "hw.physical_disk.state";

/**
  Type of the physical disk
 */
static constexpr const char *kHwPhysicalDiskType = "hw.physical_disk.type";

/**
  Location of the sensor
 */
static constexpr const char *kHwSensorLocation = "hw.sensor_location";

/**
  Serial number of the hardware component
 */
static constexpr const char *kHwSerialNumber = "hw.serial_number";

/**
  The current state of the component
 */
static constexpr const char *kHwState = "hw.state";

/**
  Type of tape drive operation
 */
static constexpr const char *kHwTapeDriveOperationType = "hw.tape_drive.operation_type";

/**
  Type of the component
  <p>
  Describes the category of the hardware component for which @code hw.state @endcode is being
  reported. For example, @code hw.type=temperature @endcode along with @code hw.state=degraded
  @endcode would indicate that the temperature of the hardware component has been reported as @code
  degraded @endcode.
 */
static constexpr const char *kHwType = "hw.type";

/**
  Vendor name of the hardware component
 */
static constexpr const char *kHwVendor = "hw.vendor";

namespace HwBatteryStateValues
{
/**
  Charging
 */
static constexpr const char *kCharging = "charging";

/**
  Discharging
 */
static constexpr const char *kDischarging = "discharging";

}  // namespace HwBatteryStateValues

namespace HwGpuTaskValues
{
/**
  Decoder
 */
static constexpr const char *kDecoder = "decoder";

/**
  Encoder
 */
static constexpr const char *kEncoder = "encoder";

/**
  General
 */
static constexpr const char *kGeneral = "general";

}  // namespace HwGpuTaskValues

namespace HwLimitTypeValues
{
/**
  Critical
 */
static constexpr const char *kCritical = "critical";

/**
  Degraded
 */
static constexpr const char *kDegraded = "degraded";

/**
  High Critical
 */
static constexpr const char *kHighCritical = "high.critical";

/**
  High Degraded
 */
static constexpr const char *kHighDegraded = "high.degraded";

/**
  Low Critical
 */
static constexpr const char *kLowCritical = "low.critical";

/**
  Low Degraded
 */
static constexpr const char *kLowDegraded = "low.degraded";

/**
  Maximum
 */
static constexpr const char *kMax = "max";

/**
  Throttled
 */
static constexpr const char *kThrottled = "throttled";

/**
  Turbo
 */
static constexpr const char *kTurbo = "turbo";

}  // namespace HwLimitTypeValues

namespace HwLogicalDiskStateValues
{
/**
  Used
 */
static constexpr const char *kUsed = "used";

/**
  Free
 */
static constexpr const char *kFree = "free";

}  // namespace HwLogicalDiskStateValues

namespace HwPhysicalDiskStateValues
{
/**
  Remaining
 */
static constexpr const char *kRemaining = "remaining";

}  // namespace HwPhysicalDiskStateValues

namespace HwStateValues
{
/**
  Degraded
 */
static constexpr const char *kDegraded = "degraded";

/**
  Failed
 */
static constexpr const char *kFailed = "failed";

/**
  Needs Cleaning
 */
static constexpr const char *kNeedsCleaning = "needs_cleaning";

/**
  OK
 */
static constexpr const char *kOk = "ok";

/**
  Predicted Failure
 */
static constexpr const char *kPredictedFailure = "predicted_failure";

}  // namespace HwStateValues

namespace HwTapeDriveOperationTypeValues
{
/**
  Mount
 */
static constexpr const char *kMount = "mount";

/**
  Unmount
 */
static constexpr const char *kUnmount = "unmount";

/**
  Clean
 */
static constexpr const char *kClean = "clean";

}  // namespace HwTapeDriveOperationTypeValues

namespace HwTypeValues
{
/**
  Battery
 */
static constexpr const char *kBattery = "battery";

/**
  CPU
 */
static constexpr const char *kCpu = "cpu";

/**
  Disk controller
 */
static constexpr const char *kDiskController = "disk_controller";

/**
  Enclosure
 */
static constexpr const char *kEnclosure = "enclosure";

/**
  Fan
 */
static constexpr const char *kFan = "fan";

/**
  GPU
 */
static constexpr const char *kGpu = "gpu";

/**
  Logical disk
 */
static constexpr const char *kLogicalDisk = "logical_disk";

/**
  Memory
 */
static constexpr const char *kMemory = "memory";

/**
  Network
 */
static constexpr const char *kNetwork = "network";

/**
  Physical disk
 */
static constexpr const char *kPhysicalDisk = "physical_disk";

/**
  Power supply
 */
static constexpr const char *kPowerSupply = "power_supply";

/**
  Tape drive
 */
static constexpr const char *kTapeDrive = "tape_drive";

/**
  Temperature
 */
static constexpr const char *kTemperature = "temperature";

/**
  Voltage
 */
static constexpr const char *kVoltage = "voltage";

}  // namespace HwTypeValues

}  // namespace hw
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
