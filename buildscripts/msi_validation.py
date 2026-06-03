# This script validates the MSI can be installed and uninstalled, and checks for
# the default install location and some expected installed files.
import codecs
import glob
import locale
import os
import subprocess
import sys
import tempfile

common_files = [
    "*\\*\\bin\\mongos.exe",
    "*\\*\\bin\\mongos.pdb",
    "*\\*\\bin\\mongod.exe",
    "*\\*\\bin\\mongod.pdb",
    "*\\*\\bin\\mongod.cfg",
    "*\\*\\bin\\InstallCompass.ps1",
    "*\\*\\MPL-2",
    "*\\*\\README",
    "*\\*\\THIRD-PARTY-NOTICES",
]

community_files = [
    "*\\*\\LICENSE-Community.txt",
]

enterprise_files = [
    "*\\*\\bin\\mongodecrypt.exe",
    "*\\*\\bin\\mongokerberos.exe",
    "*\\*\\bin\\mongoldap.exe",
    "*\\*\\bin\\sasl2.dll",
    "*\\*\\bin\\sasl2.pdb",
    "*\\*\\LICENSE-Enterprise.txt",
    "*\\*\\THIRD-PARTY-NOTICES.windows",
]


def print_log_file(log_path):
    """Print an MSI log without assuming the log or stdout encoding."""
    with open(log_path, "rb") as file:
        log_contents = file.read()

    encodings = []
    if log_contents.startswith((codecs.BOM_UTF16_LE, codecs.BOM_UTF16_BE)):
        encodings.append("utf-16")

    encodings.extend(["utf-8-sig", locale.getpreferredencoding(False), "cp1252"])

    log_text = None
    for encoding in dict.fromkeys(encodings):
        try:
            log_text = log_contents.decode(encoding)
            break
        except UnicodeDecodeError:
            continue

    if log_text is None:
        log_text = log_contents.decode("utf-8", errors="replace")

    stdout_encoding = sys.stdout.encoding or locale.getpreferredencoding(False)
    safe_log_text = log_text.encode(stdout_encoding,
                                    errors="backslashreplace").decode(stdout_encoding)
    print(safe_log_text)


def execute_msi(msi_path, install=True):
    """
    Run an MSI file to either install or uninstall.

    :param msi_path: Path to the MSI file.
    :return: None
    """
    if not os.path.exists(msi_path):
        print(f"Error: The file '{msi_path}' does not exist.")
        sys.exit(1)

    features = ["ServerNoService", "Router", "MiscellaneousTools", "InstallCompassFeature"]
    # Should look like ADDLOCAL=feature1,feature2
    features_install_string = "" if len(features) == 0 else "ADDLOCAL=" + ",".join(features)

    try:
        fd, log_path = tempfile.mkstemp(suffix=".log")
        os.close(fd)
        process_type = "Install" if install else "Uninstall"
        install_commands = [
            "msiexec",
            "/i" if install else "/x",
            msi_path,
            features_install_string,
            "/quiet",
            "/norestart",
            "/l*v",
            log_path,
        ]
        print(f"{process_type} '{msi_path}' with command: {' '.join(install_commands)}...")
        subprocess.run(install_commands, check=True)
        print(f"{process_type} completed successfully.")
        print_log_file(log_path)
    except subprocess.CalledProcessError as err:
        print(f"Error while {process_type} MSI: {err}")
        print_log_file(log_path)
        sys.exit(1)


def validate_files(is_enterprise):
    """Validate files from the MSI exist, and exe help commands launch."""
    install_path = os.path.join(os.environ.get("ProgramFiles", r"C:\Program Files"), "MongoDB")
    files_to_check = common_files + (enterprise_files if is_enterprise else community_files)
    print("Validating some of the expected files exist in Program Files install directory.")
    for file in files_to_check:
        file_path = os.path.join(install_path, file)
        print(f"Checking if {file_path} exists.")
        file_match = glob.glob(file_path)
        if file_match:
            print(f"File exists: {file_match[0]}")
            if file_match[0].endswith(".exe"):
                validate_help(file_match[0])
        else:
            print(f"Error: {file_path} could not be found.")
            sys.exit(1)


def validate_help(exe_path):
    try:
        install_commands = [exe_path, "--help"]
        print(f"Calling '{exe_path}' with command: {' '.join(install_commands)}...")
        subprocess.run(install_commands, check=True)
        print(f"{exe_path} called help successfully.")
    except subprocess.CalledProcessError as err:
        print(f"Error while calling help for {exe_path}: {err}")
        sys.exit(1)


def main():
    if len(sys.argv) != 2:
        print("Usage: python msi_validation.py <path_to_msi>")
        sys.exit(1)

    msi_path = sys.argv[1]
    is_enterprise = "enterprise" in os.path.basename(msi_path).lower()

    execute_msi(msi_path, install=True)
    validate_files(is_enterprise)
    execute_msi(msi_path, install=False)


if __name__ == "__main__":
    main()
