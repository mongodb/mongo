"""Static checks that the GCP OIDC helper environment is hash-locked.

The external_auth_oidc_gcp Evergreen task installs google-cloud-compute and its
transitive dependencies while GCP credentials are on disk. To prevent any
index-supplied content from being installed and executed, the helper scripts
must only install from the pinned requirements file with pip's
--require-hashes enforcement, and every requirement line must carry at least
one sha256 hash.
"""

import re
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
EVERGREEN_DIR = REPO_ROOT / "evergreen"

VENV_SCRIPT = "evergreen/external_auth_gcp_venv.sh"
SETUP_SCRIPT = "evergreen/external_auth_gcp_setup.sh"
TEARDOWN_SCRIPT = "evergreen/external_auth_gcp_teardown.sh"
REQUIREMENTS_FILE = "evergreen/external_auth_gcp_requirements.txt"

REQUIREMENT_LINE = re.compile(r"^(?P<name>[A-Za-z0-9._-]+)==(?P<version>[^\s]+)")
HASH_PATTERN = re.compile(r"--hash=sha256:[0-9a-fA-F]{64}(?![0-9a-fA-F])")


class TestExternalAuthGcpRequirements(unittest.TestCase):
    @staticmethod
    def _read(repo_relative_path: str) -> str:
        path = REPO_ROOT / repo_relative_path
        assert path.exists(), f"missing required file: {repo_relative_path}"
        return path.read_text()

    def test_helper_scripts_exist(self):
        # Guard so the check cannot pass on missing files.
        for script in (VENV_SCRIPT, SETUP_SCRIPT, TEARDOWN_SCRIPT, REQUIREMENTS_FILE):
            self.assertTrue((REPO_ROOT / script).exists(), f"missing file: {script}")

    def test_setup_and_teardown_run_vm_manager(self):
        # The scripts must still provision and clean up the GCE VM, so the
        # check cannot pass on deleted functionality.
        self.assertIn("gce_vm_manager.py create", self._read(SETUP_SCRIPT))
        self.assertIn("gce_vm_manager.py delete", self._read(TEARDOWN_SCRIPT))

    def test_no_pip_install_in_setup_or_teardown(self):
        # All GCP dependency installation must flow through the isolated
        # helper in the venv script; the entry scripts must not install.
        for script in (SETUP_SCRIPT, TEARDOWN_SCRIPT):
            self.assertNotIn(
                "pip install", self._read(script), f"unexpected pip install in {script}"
            )

    def test_venv_install_is_hash_enforced(self):
        venv_contents = self._read(VENV_SCRIPT)
        install_calls = re.findall(r"pip install[^\n]*", venv_contents)
        self.assertGreater(len(install_calls), 0, "no pip install found in venv helper")
        for call in install_calls:
            self.assertIn("--require-hashes", call, f"pip install is not hash enforced: {call}")
            # Source distributions must be refused entirely: --require-hashes
            # does not cover the build dependencies pip downloads for PEP 517
            # build isolation when building an sdist.
            self.assertIn(
                "--only-binary=:all:",
                call,
                f"pip install does not forbid source distributions: {call}",
            )
            self.assertIn(
                f"-r {REQUIREMENTS_FILE}",
                call,
                f"pip install does not use the pinned requirements file: {call}",
            )

    def test_requirements_fully_pinned_and_hashed(self):
        lines = [
            line.strip()
            for line in self._read(REQUIREMENTS_FILE).splitlines()
            if line.strip() and not line.strip().startswith("#")
        ]
        self.assertGreater(len(lines), 0, "requirements file is empty")
        seen = set()
        for line in lines:
            match = REQUIREMENT_LINE.match(line)
            self.assertIsNotNone(match, f"requirement is not pinned with ==: {line}")
            name = match.group("name").lower().replace("_", "-")
            self.assertNotIn(name, seen, f"duplicate requirement: {name}")
            seen.add(name)
            self.assertTrue(
                HASH_PATTERN.search(line), f"requirement has no sha256 --hash entry: {line}"
            )


if __name__ == "__main__":
    unittest.main()
