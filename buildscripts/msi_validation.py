# This script validates the msi can be installed, uninstalled, and checks for the default install location and some of the possible install files
import glob
import os
import re
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
        temp_log = tempfile.NamedTemporaryFile(mode="w", suffix=".log", delete=False)
        process_type = "Install" if install else "Uninstall"
        install_commands = [
            "msiexec",
            "/i" if install else "/x",
            msi_path,
            features_install_string,
            "/quiet",
            "/norestart",
            "/l*v",
            temp_log.name,
        ]
        print(f"{process_type} '{msi_path}' with command: {' '.join(install_commands)}...")
        subprocess.run(install_commands, check=True)
        print(f"{process_type} completed successfully.")
        with open(temp_log.name, "r") as file:
            print(file.read())
    except subprocess.CalledProcessError as e:
        print(f"Error while {process_type} MSI: {e}")
        with open(temp_log.name, "r") as file:
            print(file.read())
        sys.exit(1)


def validate_files(is_enterprise):
    """
    Validate the files exist from the msi, and if its an exe the help command launches
    """
    install_path = os.path.join(os.environ.get("ProgramFiles"), "MongoDB")
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
            if file_match[0].endswith("mongod.exe"):
                validate_version(file_match[0])
        else:
            print(f"Error: {file_path} could not be found.")
            sys.exit(1)


def validate_help(exe_path):
    try:
        install_commands = [exe_path, "--help"]
        print(f"Calling '{exe_path}' with command: {' '.join(install_commands)}...")
        subprocess.run(install_commands, check=True)
        print(f"{exe_path} called help successfully.")
    except subprocess.CalledProcessError as e:
        print(f"Error while calling help for {exe_path}: {e}")
        sys.exit(1)

# Make sure we have a proper git version in the windows release
def validate_version(exe_path):
    try:
        version_command = [exe_path, "--version"]
        print(f"Calling '{exe_path}' with command: {' '.join(version_command)}...")
        result = subprocess.run(version_command, check=True, stdout=subprocess.PIPE, text=True)
        print(f"{exe_path} called version successfully.")
        match = re.search('.*"gitVersion": "[0-9a-fA-F]{40}".*', result.stdout)
        if match:
            print("Found a valid git version.")
            return
        else:
            print("--version command did not contain a valid git version in gitVersion. Stdout:")
            print(result.stdout)
            sys.exit(1)
    except subprocess.CalledProcessError as e:
        print(f"Error while calling version for {exe_path}: {e}")
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
