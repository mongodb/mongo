"""Check native PPC test dependencies and ARM64 release build scheduling."""

import unittest

from buildscripts.tests import test_evergreen_s390x_cross as cross_tests


class Ppc64leCrossSchedulingTest(cross_tests.S390xCrossSchedulingTest):
    STATIC_VARIANTS = {
        "enterprise-rhel-81-ppc64le": "rhel81-power8-large",
        "enterprise-rhel-9-ppc64le": "rhel9-power-large",
        "enterprise-rhel-10-ppc64le": "rhel10-power-large",
    }
    ARM_DISTROS = {
        "enterprise-rhel-81-ppc64le": "rhel8.8-arm64-m8g-4xlarge",
        "enterprise-rhel-9-ppc64le": "rhel93-arm64-m8g-4xlarge",
        "enterprise-rhel-10-ppc64le": "rhel10.0-arm64-large",
    }
    SMOKE_TASK = "ppc64le_cross_toolchain_smoke"
    CROSS_PACKAGE_GROUP = "compile_all_and_package_serial_no_unittests_TG"
    INTEGRATION_GROUP = "compile_integration_and_test_no_audit_parallel_stream_TG"


if __name__ == "__main__":
    unittest.main()
