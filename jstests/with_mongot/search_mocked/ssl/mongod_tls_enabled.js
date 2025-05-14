/**
 * Test the basic operation of a `$search` aggregation stage while TLS is enabled on mongod.
 */
import {
    verifyTLSConfigurationFails,
    verifyTLSConfigurationPasses
} from "jstests/with_mongot/search_mocked/ssl/lib/search_tls_utils.js";

// These tests confirm that mongod is communicating with mongot using TLS since mongot is set with
// "requireTLS" which means that incoming connections must use TLS.
verifyTLSConfigurationPasses(
    {mongotMockTLSMode: "requireTLS", mongodTLSMode: "preferTLS", searchTLSMode: "globalTLS"});
verifyTLSConfigurationPasses(
    {mongotMockTLSMode: "requireTLS", mongodTLSMode: "requireTLS", searchTLSMode: "globalTLS"});

// Remaining tests for testing 'globalTLS' for 'searchTLSMode'
// The following combinations should all pass.
verifyTLSConfigurationPasses(
    {mongotMockTLSMode: "disabled", mongodTLSMode: "allowTLS", searchTLSMode: "globalTLS"});

verifyTLSConfigurationPasses(
    {mongotMockTLSMode: "allowTLS", mongodTLSMode: "allowTLS", searchTLSMode: "globalTLS"});
verifyTLSConfigurationPasses(
    {mongotMockTLSMode: "allowTLS", mongodTLSMode: "preferTLS", searchTLSMode: "globalTLS"});
verifyTLSConfigurationPasses(
    {mongotMockTLSMode: "allowTLS", mongodTLSMode: "requireTLS", searchTLSMode: "globalTLS"});

verifyTLSConfigurationPasses(
    {mongotMockTLSMode: "preferTLS", mongodTLSMode: "allowTLS", searchTLSMode: "globalTLS"});
verifyTLSConfigurationPasses(
    {mongotMockTLSMode: "preferTLS", mongodTLSMode: "preferTLS", searchTLSMode: "globalTLS"});
verifyTLSConfigurationPasses(
    {mongotMockTLSMode: "preferTLS", mongodTLSMode: "requireTLS", searchTLSMode: "globalTLS"});

// The following combinations should fail since mongod is not communicating in the way
// mongot expects (sending TLS when it's not enabled, or not sending when it's required).
verifyTLSConfigurationFails(
    {mongotMockTLSMode: "disabled", mongodTLSMode: "preferTLS", searchTLSMode: "globalTLS"});
verifyTLSConfigurationFails(
    {mongotMockTLSMode: "disabled", mongodTLSMode: "requireTLS", searchTLSMode: "globalTLS"});

verifyTLSConfigurationFails(
    {mongotMockTLSMode: "requireTLS", mongodTLSMode: "allowTLS", searchTLSMode: "globalTLS"});

// Test the remaining combinations with searchTLSMode set to "disabled" or "allowTLS" (doesn't use
// TLS for egress connections to mongot). If mongot doesn't 'requireTLS', the tests should pass.
for (const mode of ["disabled", "allowTLS"]) {
    // Confirm we fail as expected since we don't use the TLS protocoleven when TLS is enabled on
    // mongod.
    verifyTLSConfigurationFails(
        {mongotMockTLSMode: "requireTLS", mongodTLSMode: "requireTLS", searchTLSMode: mode});
    verifyTLSConfigurationFails(
        {mongotMockTLSMode: "requireTLS", mongodTLSMode: "preferTLS", searchTLSMode: mode});

    // Test the remaining options.
    verifyTLSConfigurationPasses(
        {mongotMockTLSMode: "allowTLS", mongodTLSMode: "allowTLS", searchTLSMode: mode});
    verifyTLSConfigurationPasses(
        {mongotMockTLSMode: "allowTLS", mongodTLSMode: "preferTLS", searchTLSMode: mode});
    verifyTLSConfigurationPasses(
        {mongotMockTLSMode: "allowTLS", mongodTLSMode: "requireTLS", searchTLSMode: mode});

    verifyTLSConfigurationPasses(
        {mongotMockTLSMode: "preferTLS", mongodTLSMode: "allowTLS", searchTLSMode: mode});
    verifyTLSConfigurationPasses(
        {mongotMockTLSMode: "preferTLS", mongodTLSMode: "preferTLS", searchTLSMode: mode});
    verifyTLSConfigurationPasses(
        {mongotMockTLSMode: "preferTLS", mongodTLSMode: "requireTLS", searchTLSMode: mode});

    verifyTLSConfigurationPasses(
        {mongotMockTLSMode: "disabled", mongodTLSMode: "allowTLS", searchTLSMode: mode});
    verifyTLSConfigurationPasses(
        {mongotMockTLSMode: "disabled", mongodTLSMode: "preferTLS", searchTLSMode: mode});
    verifyTLSConfigurationPasses(
        {mongotMockTLSMode: "disabled", mongodTLSMode: "requireTLS", searchTLSMode: mode});

    verifyTLSConfigurationFails(
        {mongotMockTLSMode: "requireTLS", mongodTLSMode: "allowTLS", searchTLSMode: mode});
}

// Test the combinations with searchTLSMode set to "preferTLS" and "requireTLS" (uses TLS for egress
// communication to mongot). If mongotmock doesn't have TLS disabled, the tests should pass.
for (const mode of ["preferTLS", "requireTLS"]) {
    verifyTLSConfigurationFails(
        {mongotMockTLSMode: "disabled", mongodTLSMode: "allowTLS", searchTLSMode: mode});
    verifyTLSConfigurationFails(
        {mongotMockTLSMode: "disabled", mongodTLSMode: "preferTLS", searchTLSMode: mode});
    verifyTLSConfigurationFails(
        {mongotMockTLSMode: "disabled", mongodTLSMode: "requireTLS", searchTLSMode: mode});

    verifyTLSConfigurationPasses(
        {mongotMockTLSMode: "allowTLS", mongodTLSMode: "allowTLS", searchTLSMode: mode});
    verifyTLSConfigurationPasses(
        {mongotMockTLSMode: "allowTLS", mongodTLSMode: "preferTLS", searchTLSMode: mode});
    verifyTLSConfigurationPasses(
        {mongotMockTLSMode: "allowTLS", mongodTLSMode: "requireTLS", searchTLSMode: mode});

    verifyTLSConfigurationPasses(
        {mongotMockTLSMode: "preferTLS", mongodTLSMode: "allowTLS", searchTLSMode: mode});
    verifyTLSConfigurationPasses(
        {mongotMockTLSMode: "preferTLS", mongodTLSMode: "preferTLS", searchTLSMode: mode});
    verifyTLSConfigurationPasses(
        {mongotMockTLSMode: "preferTLS", mongodTLSMode: "requireTLS", searchTLSMode: mode});

    verifyTLSConfigurationPasses(
        {mongotMockTLSMode: "requireTLS", mongodTLSMode: "allowTLS", searchTLSMode: mode});
    verifyTLSConfigurationPasses(
        {mongotMockTLSMode: "requireTLS", mongodTLSMode: "preferTLS", searchTLSMode: mode});
    verifyTLSConfigurationPasses(
        {mongotMockTLSMode: "requireTLS", mongodTLSMode: "requireTLS", searchTLSMode: mode});
}

// Confirm that the default of searchTLSMode is "disabled" so this should fail.
// TODO SERVER-99787 update this test.
verifyTLSConfigurationFails({mongotMockTLSMode: "requireTLS", mongodTLSMode: "requireTLS"});
