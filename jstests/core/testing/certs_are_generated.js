/**
 * Tests that certificate generation is always complete when running resmoke tests.
 * @tags: [
 *   # This test relies on the installation directory structure, and thus does not work properly on
 *   # SELinux.
 *   no_selinux,
 * ]
 */

let installDir = _getEnv("INSTALL_DIR");
if (installDir === "") {
    installDir = ".";
}
const pathsep = _isWindows() ? "\\" : "/";
const certDir = installDir + pathsep + "x509";
jsTest.log.info(certDir);

jsTest.log.info(ls(installDir));
jsTest.log.info(ls(certDir));

assert(fileExists(certDir + pathsep + "ca.pem"));
assert(fileExists(certDir + pathsep + "crl.pem.digest.sha1"));
