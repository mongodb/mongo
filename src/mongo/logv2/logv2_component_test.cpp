/**
 *    Copyright (C) 2020-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_component_settings.h"
#include "mongo/unittest/unittest.h"


namespace mongo::logv2 {
namespace {

// Constants for log component test cases.
const LogComponent componentDefault = LogComponent::kDefault;
const LogComponent componentA = LogComponent::kCommand;
const LogComponent componentB = LogComponent::kAccessControl;
const LogComponent componentC = LogComponent::kNetwork;
const LogComponent componentD = LogComponent::kStorage;
const LogComponent componentE = LogComponent::kJournal;

TEST(LogV2ComponentSettingsTest, MinimumLogSeverity) {
    LogComponentSettings settings;
    ASSERT_TRUE(settings.hasMinimumLogSeverity(LogComponent::kDefault));
    ASSERT_TRUE(settings.getMinimumLogSeverity(LogComponent::kDefault) == LogSeverity::Log());
    for (int i = 0; i < int(LogComponent::kNumLogComponents); ++i) {
        LogComponent component = static_cast<LogComponent::Value>(i);
        if (component == LogComponent::kDefault) {
            continue;
        }
        ASSERT_FALSE(settings.hasMinimumLogSeverity(component));
    }

    // Override and clear minimum severity level.
    for (int i = 0; i < int(LogComponent::kNumLogComponents); ++i) {
        LogComponent component = static_cast<LogComponent::Value>(i);
        LogSeverity severity = LogSeverity::Debug(2);

        // Override severity level.
        settings.setMinimumLoggedSeverity(component, severity);
        ASSERT_TRUE(settings.hasMinimumLogSeverity(component));
        ASSERT_TRUE(settings.getMinimumLogSeverity(component) == severity);

        // Clear severity level.
        // Special case: when clearing LogComponent::kDefault, the corresponding
        //               severity level is set to default values (ie. Log()).
        settings.clearMinimumLoggedSeverity(component);
        if (component == LogComponent::kDefault) {
            ASSERT_TRUE(settings.hasMinimumLogSeverity(component));
            ASSERT_TRUE(settings.getMinimumLogSeverity(LogComponent::kDefault) ==
                        LogSeverity::Log());
        } else {
            ASSERT_FALSE(settings.hasMinimumLogSeverity(component));
        }
    }
}

// Test for shouldLog() when the minimum logged severity is set only for LogComponent::kDefault.
TEST(LogV2ComponentSettingsTest, ShouldLogDefaultLogComponentOnly) {
    LogComponentSettings settings;

    // Initial log severity for LogComponent::kDefault is Log().
    // Other components should get the same outcome as kDefault.
    ASSERT_TRUE(settings.shouldLog(LogComponent::kDefault, LogSeverity::Info()));
    ASSERT_TRUE(settings.shouldLog(LogComponent::kDefault, LogSeverity::Log()));
    ASSERT_FALSE(settings.shouldLog(LogComponent::kDefault, LogSeverity::Debug(1)));
    ASSERT_FALSE(settings.shouldLog(LogComponent::kDefault, LogSeverity::Debug(2)));

    ASSERT_TRUE(settings.shouldLog(componentA, LogSeverity::Info()));
    ASSERT_TRUE(settings.shouldLog(componentA, LogSeverity::Log()));
    ASSERT_FALSE(settings.shouldLog(componentA, LogSeverity::Debug(1)));
    ASSERT_FALSE(settings.shouldLog(componentA, LogSeverity::Debug(2)));

    // Set minimum logged severity so that Debug(1) messages are written to log domain.
    settings.setMinimumLoggedSeverity(LogComponent::kDefault, LogSeverity::Debug(1));

    ASSERT_TRUE(settings.shouldLog(LogComponent::kDefault, LogSeverity::Info()));
    ASSERT_TRUE(settings.shouldLog(LogComponent::kDefault, LogSeverity::Log()));
    ASSERT_TRUE(settings.shouldLog(LogComponent::kDefault, LogSeverity::Debug(1)));
    ASSERT_FALSE(settings.shouldLog(LogComponent::kDefault, LogSeverity::Debug(2)));
    // Same results for non-default components.
    ASSERT_TRUE(settings.shouldLog(componentA, LogSeverity::Info()));
    ASSERT_TRUE(settings.shouldLog(componentA, LogSeverity::Log()));
    ASSERT_TRUE(settings.shouldLog(componentA, LogSeverity::Debug(1)));
    ASSERT_FALSE(settings.shouldLog(componentA, LogSeverity::Debug(2)));
}

// Test for shouldLog() when we have configured a single component.
// Also checks that severity level has been reverted to match LogComponent::kDefault
// after clearing level.
// Minimum severity levels:
// LogComponent::kDefault: 1
// componentA: 2
TEST(LogV2ComponentSettingsTest, ShouldLogSingleComponent) {
    LogComponentSettings settings;

    settings.setMinimumLoggedSeverity(LogComponent::kDefault, LogSeverity::Debug(1));
    settings.setMinimumLoggedSeverity(componentA, LogSeverity::Debug(2));

    // Components for log message: componentA only.
    ASSERT_TRUE(settings.shouldLog(componentA, LogSeverity::Debug(2)));
    ASSERT_FALSE(settings.shouldLog(componentA, LogSeverity::Debug(3)));

    // Clear severity level for componentA and check shouldLog() again.
    settings.clearMinimumLoggedSeverity(componentA);
    ASSERT_TRUE(settings.shouldLog(componentA, LogSeverity::Debug(1)));
    ASSERT_FALSE(settings.shouldLog(componentA, LogSeverity::Debug(2)));
}

// Test for shouldLog() when we have configured multiple components.
// Minimum severity levels:
// LogComponent::kDefault: 1
// componentA: 2
// componentB: 0
TEST(LogV2ComponentSettingsTest, ShouldLogMultipleComponentsConfigured) {
    LogComponentSettings settings;

    settings.setMinimumLoggedSeverity(LogComponent::kDefault, LogSeverity::Debug(1));
    settings.setMinimumLoggedSeverity(componentA, LogSeverity::Debug(2));
    settings.setMinimumLoggedSeverity(componentB, LogSeverity::Log());

    // Components for log message: componentA only.
    ASSERT_TRUE(settings.shouldLog(componentA, LogSeverity::Debug(2)));
    ASSERT_FALSE(settings.shouldLog(componentA, LogSeverity::Debug(3)));

    // Components for log message: componentB only.
    ASSERT_TRUE(settings.shouldLog(componentB, LogSeverity::Log()));
    ASSERT_FALSE(settings.shouldLog(componentB, LogSeverity::Debug(1)));

    // Components for log message: componentC only.
    // Since a component-specific minimum severity is not configured for componentC,
    // shouldLog() falls back on LogComponent::kDefault.
    ASSERT_TRUE(settings.shouldLog(componentC, LogSeverity::Debug(1)));
    ASSERT_FALSE(settings.shouldLog(componentC, LogSeverity::Debug(2)));
}

// Log component hierarchy.
TEST(LogV2ComponentTest, Hierarchy) {
    // Parent component is not meaningful for kDefault and kNumLogComponents.
    ASSERT_EQUALS(LogComponent::kNumLogComponents, LogComponent(LogComponent::kDefault).parent());
    ASSERT_EQUALS(LogComponent::kNumLogComponents,
                  LogComponent(LogComponent::kNumLogComponents).parent());

    // Default -> ComponentD -> ComponentE
    ASSERT_EQUALS(LogComponent::kDefault, LogComponent(componentD).parent());
    ASSERT_EQUALS(componentD, LogComponent(componentE).parent());
    ASSERT_NOT_EQUALS(LogComponent::kDefault, LogComponent(componentE).parent());

    // Log components should inherit parent's log severity in settings.
    LogComponentSettings settings;
    settings.setMinimumLoggedSeverity(LogComponent::kDefault, LogSeverity::Debug(1));
    settings.setMinimumLoggedSeverity(componentD, LogSeverity::Debug(2));

    // componentE should inherit componentD's log severity.
    ASSERT_TRUE(settings.shouldLog(componentE, LogSeverity::Debug(2)));
    ASSERT_FALSE(settings.shouldLog(componentE, LogSeverity::Debug(3)));

    // Clearing parent's log severity - componentE should inherit from Default.
    settings.clearMinimumLoggedSeverity(componentD);
    ASSERT_TRUE(settings.shouldLog(componentE, LogSeverity::Debug(1)));
    ASSERT_FALSE(settings.shouldLog(componentE, LogSeverity::Debug(2)));
}

// Dotted name of component includes names of ancestors.
TEST(LogV2ComponentTest, DottedNameOldTest) {
    // Default -> ComponentD -> ComponentE
    ASSERT_EQUALS(componentDefault.getShortName(),
                  LogComponent(LogComponent::kDefault).getDottedName());
    ASSERT_EQUALS(componentD.getShortName(), componentD.getDottedName());
    ASSERT_EQUALS(componentD.getShortName() + "." + componentE.getShortName(),
                  componentE.getDottedName());
}

TEST(LogV2ComponentTest, DottedName) {
    struct {
        LogComponent lc;
        StringData expect;
    } const kSpecs[] = {
        {LogComponent::kDefault, "default"},
        {LogComponent::kAccessControl, "accessControl"},
        {LogComponent::kReplication, "replication"},
        {LogComponent::kReplicationElection, "replication.election"},
        {LogComponent::kNumLogComponents, "total"},
    };
    for (auto&& spec : kSpecs) {
        ASSERT_EQ(spec.lc.getDottedName(), spec.expect) << spec.lc;
    }
}


}  // namespace
}  // namespace mongo::logv2
