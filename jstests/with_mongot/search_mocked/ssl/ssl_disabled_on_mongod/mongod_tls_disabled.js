/**
 * Test the basic operation of a `$search` aggregation stage for various settings of mongot using
 * TLS, while TLS is not enabled on mongod.
 *
 * This test cannot be run in the search_ssl suite since that suite automatically enables TLS for
 * connections to mongod, which is incompatible with mongod having TLS disabled.
 */
import {
    CLIENT_CERT,
} from "jstests/ssl/libs/ssl_helpers.js";
import {
    verifyTLSConfigurationFails,
    verifyTLSConfigurationPasses
} from "jstests/with_mongot/search_mocked/ssl/lib/search_tls_utils.js";

verifyTLSConfigurationPasses(
    {mongotMockTLSMode: "disabled", mongodTLSMode: "disabled", searchTLSMode: "globalTLS"});
verifyTLSConfigurationPasses(
    {mongotMockTLSMode: "preferTLS", mongodTLSMode: "disabled", searchTLSMode: "globalTLS"});
verifyTLSConfigurationPasses(
    {mongotMockTLSMode: "allowTLS", mongodTLSMode: "disabled", searchTLSMode: "globalTLS"});
// This fails since mongod doesn't communicate with mongotmock using TLS when it has to.
verifyTLSConfigurationFails(
    {mongotMockTLSMode: "requireTLS", mongodTLSMode: "disabled", searchTLSMode: "globalTLS"});

// Test that setting 'searchTLSMode' to 'disabled' or 'allowTLS' has the same behavior.
for (const mode of ["disabled", "allowTLS"]) {
    verifyTLSConfigurationPasses(
        {mongotMockTLSMode: "disabled", mongodTLSMode: "disabled", searchTLSMode: mode});
    verifyTLSConfigurationPasses(
        {mongotMockTLSMode: "preferTLS", mongodTLSMode: "disabled", searchTLSMode: mode});
    verifyTLSConfigurationPasses(
        {mongotMockTLSMode: "allowTLS", mongodTLSMode: "disabled", searchTLSMode: mode});
    verifyTLSConfigurationFails(
        {mongotMockTLSMode: "requireTLS", mongodTLSMode: "disabled", searchTLSMode: mode});
}

// Test that nonsense searchTLSMode will error.
clearRawMongoProgramOutput();
assert.throws(() => MongoRunner.runMongod({setParameter: {searchTLSMode: "randomValue"}}));
assert(rawMongoProgramOutput(".*").includes(
    "searchTLSMode must be one of: (globalTLS|disabled|allowTLS|preferTLS|requireTLS)." +
    " Input was: randomValue"));

// Test that setting searchTLSMode to enabled without TLS will error.
for (const mode of ["preferTLS", "requireTLS"]) {
    clearRawMongoProgramOutput();
    assert.throws(() => MongoRunner.runMongod({setParameter: {searchTLSMode: mode}}));
    assert(rawMongoProgramOutput(".*").includes(
        "searchTLSMode set to enable TLS for connecting to mongot (preferTLS or requireTLS), " +
        "but no TLS certificate provided. Please specify net.tls.certificateKeyFile."));
}

// Test that certificate for TLS must be accompanied by TLS being enabled. If this changes, make
// sure this setting combination now works to communicate with a mongot that "requiresTLS".
clearRawMongoProgramOutput();
assert.throws(
    () => MongoRunner.runMongod(
        {tlsCertificateKeyFile: CLIENT_CERT, setParameter: {searchTLSMode: "requireTLS"}}));
assert(rawMongoProgramOutput(".*").includes(
    "need to enable TLS via the sslMode/tlsMode flag when using TLS configuration parameters"));
