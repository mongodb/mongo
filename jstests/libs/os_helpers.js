/**
 * Test helpers for identifying OS
 */

function isRHEL8() {
    if (_isWindows()) {
        return false;
    }

    // RHEL 8 disables TLS 1.0 and TLS 1.1 as part their default crypto policy
    // We skip tests on RHEL 8 that require these versions as a result.
    const grep_result = runProgram('grep', 'Ootpa', '/etc/redhat-release');
    if (grep_result == 0) {
        return true;
    }

    return false;
}

function isSUSE15SP1() {
    if (_isWindows()) {
        return false;
    }

    // SUSE 15 SP1 FIPS module does not work. SP2 does work.
    // The FIPS code returns FIPS_R_IN_ERROR_STATE in what is likely a race condition
    // since it only happens in sharded clusters.
    const grep_result = runProgram('grep', '15-SP1', '/etc/os-release');
    if (grep_result == 0) {
        return true;
    }

    return false;
}

function isUbuntu() {
    if (_isWindows()) {
        return false;
    }

    // Ubuntu 18.04 and later compiles openldap against gnutls which does not
    // support SHA1 signed certificates. ldaptest.10gen.cc uses a SHA1 cert.
    const grep_result = runProgram('grep', 'ID=ubuntu', '/etc/os-release');
    if (grep_result == 0) {
        return true;
    }

    return false;
}

function isUbuntu1804() {
    if (_isWindows()) {
        return false;
    }

    // Ubuntu 18.04's TLS 1.3 implementation has an issue with OCSP stapling. We have disabled
    // stapling on this build variant, so we need to ensure that tests that require stapling
    // do not run on this machine.
    const grep_result = runProgram('grep', 'bionic', '/etc/os-release');
    if (grep_result === 0) {
        return true;
    }

    return false;
}

function isUbuntu2004() {
    if (_isWindows()) {
        return false;
    }

    // Ubuntu 20.04 disables TLS 1.0 and TLS 1.1 as part their default crypto policy
    // We skip tests on Ubuntu 20.04 that require these versions as a result.
    const grep_result = runProgram('grep', 'focal', '/etc/os-release');
    if (grep_result == 0) {
        return true;
    }

    return false;
}

function isDebian10() {
    if (_isWindows()) {
        return false;
    }

    // Debian 10 disables TLS 1.0 and TLS 1.1 as part their default crypto policy
    // We skip tests on Debian 10 that require these versions as a result.
    try {
        // this file exists on systemd-based systems, necessary to avoid mischaracterizing debian
        // derivatives as stock debian
        const releaseFile = cat("/etc/os-release").toLowerCase();
        const prettyName = releaseFile.split('\n').find(function(line) {
            return line.startsWith("pretty_name");
        });
        return prettyName.includes("debian") &&
            (prettyName.includes("10") || prettyName.includes("buster") ||
             prettyName.includes("bullseye"));
    } catch (e) {
        return false;
    }
}
