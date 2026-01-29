/**
 * Tests that certificate generation is always complete when running resmoke tests.
 * @tags: [
 *   # This test relies on the installation directory structure, and thus does not work properly on
 *   # SELinux.
 *   no_selinux,
 * ]
 */

const certDir = getX509Path("");
jsTest.log.info(certDir);

jsTest.log.info(ls(certDir));

assert(fileExists(getX509Path("ca.pem")));
assert(fileExists(getX509Path("crl.pem.digest.sha1")));
