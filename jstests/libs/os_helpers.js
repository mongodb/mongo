/**
 * Test helpers for identifying OS
 */

function isLinux() {
    return getBuildInfo().buildEnvironment.target_os == "linux";
}

// See "man 5 os-release" for documentation
function readOsRelease() {
    try {
        const os_release = cat("/etc/os-release");

        let lines = os_release.split("\n");

        let tags = {};

        for (let line of lines) {
            let vp = line.replaceAll("\"", "").split("=");
            tags[vp[0]] = vp[1];
        }

        return tags;
    } catch {
        // ignore
    }

    assert(!isLinux(), "Linux hosts should always have /etc/os-release.");

    return {};
}

/**
 * Check if Linux OS is given identifier. Identifiers are always lower case strings.
 *
 * @param {string} distro ID of the distro in os-release
 * @returns
 */
function isDistro(distro) {
    let tags = readOsRelease();
    return tags.hasOwnProperty("ID") && tags["ID"] === distro;
}

/**
 * Check if Linux OS is given identifier and specific version. Do not use for matching major
 * versions like RHEL 8, isRHELMajorVerison.
 *
 * @param {string} distro ID of the distro in os-release
 * @returns
 */
function isDistroVersion(distro, version) {
    let tags = readOsRelease();
    return tags.hasOwnProperty("ID") && tags["ID"] === distro &&
        tags.hasOwnProperty("VERSION_ID") && tags["VERSION_ID"] === version;
}

/**
 * Is it RHEL and is it 7, 8 or 9?
 * @param {string} majorVersion
 * @returns True if majorVersion = 8 and version is 8.1, 8.2 etc.
 */
function isRHELMajorVerison(majorVersion) {
    let tags = readOsRelease();
    return tags.hasOwnProperty("ID") && tags["ID"] === "rhel" &&
        tags.hasOwnProperty("VERSION_ID") && tags["VERSION_ID"].startsWith(majorVersion);
}

/**
 * Example
NAME="Red Hat Enterprise Linux"
VERSION="8.7 (Ootpa)"
ID="rhel"
ID_LIKE="fedora"
VERSION_ID="8.7"
PLATFORM_ID="platform:el8"
PRETTY_NAME="Red Hat Enterprise Linux 8.7 (Ootpa)"
ANSI_COLOR="0;31"
CPE_NAME="cpe:/o:redhat:enterprise_linux:8::baseos"
HOME_URL="https://www.redhat.com/"
DOCUMENTATION_URL="https://access.redhat.com/documentation/red_hat_enterprise_linux/8/"
BUG_REPORT_URL="https://bugzilla.redhat.com/"

REDHAT_BUGZILLA_PRODUCT="Red Hat Enterprise Linux 8"
REDHAT_BUGZILLA_PRODUCT_VERSION=8.7
REDHAT_SUPPORT_PRODUCT="Red Hat Enterprise Linux"
REDHAT_SUPPORT_PRODUCT_VERSION="8.7"
 */
function isRHEL8() {
    // RHEL 8 disables TLS 1.0 and TLS 1.1 as part their default crypto policy
    // We skip tests on RHEL 8 that require these versions as a result.
    return isRHELMajorVerison("8");
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
    // Ubuntu 18.04 and later compiles openldap against gnutls which does not
    // support SHA1 signed certificates. ldaptest.10gen.cc uses a SHA1 cert.
    return isDistro("ubuntu");
}

/**
 * Example:
NAME="Ubuntu"
VERSION="18.04.6 LTS (Bionic Beaver)"
ID=ubuntu
ID_LIKE=debian
PRETTY_NAME="Ubuntu 18.04.6 LTS"
VERSION_ID="18.04"
HOME_URL="https://www.ubuntu.com/"
SUPPORT_URL="https://help.ubuntu.com/"
BUG_REPORT_URL="https://bugs.launchpad.net/ubuntu/"
PRIVACY_POLICY_URL="https://www.ubuntu.com/legal/terms-and-policies/privacy-policy"
VERSION_CODENAME=bionic
UBUNTU_CODENAME=bionic
 */
function isUbuntu1804() {
    // Ubuntu 18.04's TLS 1.3 implementation has an issue with OCSP stapling. We have disabled
    // stapling on this build variant, so we need to ensure that tests that require stapling
    // do not run on this machine.
    return isDistroVersion("ubuntu", "18.04");
}

function isUbuntu2004() {
    // Ubuntu 20.04 disables TLS 1.0 and TLS 1.1 as part their default crypto policy
    // We skip tests on Ubuntu 20.04 that require these versions as a result.
    return isDistroVersion("ubuntu", "20.04");
}

/**
 * Example:
PRETTY_NAME="Debian GNU/Linux 12 (bookworm)"
NAME="Debian GNU/Linux"
VERSION_ID="12"
VERSION="12 (bookworm)"
VERSION_CODENAME=bookworm
ID=debian
HOME_URL="https://www.debian.org/"
SUPPORT_URL="https://www.debian.org/support"
BUG_REPORT_URL="https://bugs.debian.org/"
 */
function isDebian() {
    return isDistro("debian");
}

/**
 * Example:
NAME="Fedora Linux"
VERSION="38 (Workstation Edition)"
ID=fedora
VERSION_ID=38
VERSION_CODENAME=""
PLATFORM_ID="platform:f38"
PRETTY_NAME="Fedora Linux 38 (Workstation Edition)"
ANSI_COLOR="0;38;2;60;110;180"
LOGO=fedora-logo-icon
CPE_NAME="cpe:/o:fedoraproject:fedora:38"
DEFAULT_HOSTNAME="fedora"
HOME_URL="https://fedoraproject.org/"
DOCUMENTATION_URL="https://docs.fedoraproject.org/en-US/fedora/f38/system-administrators-guide/"
SUPPORT_URL="https://ask.fedoraproject.org/"
BUG_REPORT_URL="https://bugzilla.redhat.com/"
REDHAT_BUGZILLA_PRODUCT="Fedora"
REDHAT_BUGZILLA_PRODUCT_VERSION=38
REDHAT_SUPPORT_PRODUCT="Fedora"
REDHAT_SUPPORT_PRODUCT_VERSION=38
SUPPORT_END=2024-05-14
VARIANT="Workstation Edition"
VARIANT_ID=workstation
 */
function isFedora() {
    return isDistro("fedora");
}

/**
 * Note: Amazon 2022 was never released for production. It became Amazon 2023.
 *
 * Example:
NAME="Amazon Linux"
VERSION="2022"
ID="amzn"
ID_LIKE="fedora"
VERSION_ID="2022"
PLATFORM_ID="platform:al2022"
PRETTY_NAME="Amazon Linux 2022"
ANSI_COLOR="0;33"
CPE_NAME="cpe:2.3:o:amazon:amazon_linux:2022"
HOME_URL="https://aws.amazon.com/linux/"
BUG_REPORT_URL="https://github.com/amazonlinux/amazon-linux-2022"
*/
function isAmazon2023() {
    return isDistroVersion("amzn", "2022") || isDistroVersion("amzn", "2023");
}
