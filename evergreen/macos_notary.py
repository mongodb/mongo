import argparse
import os
import platform
import shutil
import urllib.request
import subprocess
import zipfile
import stat
import sys
import yaml


if platform.system().lower() != 'darwin':
    print("Not a macos system, skipping macos signing.")
    sys.exit(0)

supported_archs = {
    'arm64': 'arm64',
    'x86_64': 'amd64'
}
arch = platform.uname().machine.lower()

if arch not in supported_archs:
    print(f"Unsupported platform uname arch: {arch}, must be {supported_archs.keys()}")
    sys.exit(1)

expansions_file = "../expansions.yml"
if not os.path.exists(expansions_file):
    print("Evergreen expansions file not found. Skipping macos_notary.")
    sys.exit(0)

with open(expansions_file) as file:
    expansions = yaml.safe_load(file)

should_sign = expansions.get("sign_macos_archive", None)
if not should_sign:
    print("sign_macos_archive expansion not found not found or false. Skipping macos_notary.")
    sys.exit(0)

macnotary_name = f'darwin_{supported_archs[arch]}'

macnotary_url = f'https://macos-notary-1628249594.s3.amazonaws.com/releases/client/latest/{macnotary_name}.zip'
print(f'Fetching macnotary tool from: {macnotary_url}')
local_filename, headers = urllib.request.urlretrieve(macnotary_url, f'{macnotary_name}.zip')
with zipfile.ZipFile(f'{macnotary_name}.zip') as zipf:
    zipf.extractall()

st = os.stat(f'{macnotary_name}/macnotary')
os.chmod(f'{macnotary_name}/macnotary', st.st_mode | stat.S_IEXEC)

failed = False
parser = argparse.ArgumentParser(
    prog="MacOS Notary",
    description="Sign and/or notarize a tarball containing unsigned binaries.",
)
parser.add_argument("--archive-name", "-a", action="store", required=True)
parser.add_argument("--entitlements-file", "-e", action="store", required=True)
parser.add_argument("--signing-type", "-s", action="store", required=True)
args = parser.parse_args()
archive_name = args.archive_name
entitlements_file = args.entitlements_file
signing_type = args.signing_type

archive_base, archive_ext = os.path.splitext(archive_name)
unsigned_archive = f'{archive_base}_unsigned{archive_ext}'
shutil.move(archive_name, unsigned_archive)

signing_cmd = [
    f'./{macnotary_name}/macnotary',
    '-f', f'{unsigned_archive}',
    '-m', f'{signing_type}',
    '-u', 'https://dev.macos-notary.build.10gen.cc/api',
    '-k', 'server',
    '--entitlements', entitlements_file,
    '--verify',
    '-b', 'server.mongodb.com',
    '-i', f'{expansions["task_id"]}',
    '-c', f'{expansions["project"]}',
    '-o', f'{archive_name}'
]

signing_env = os.environ.copy()
signing_env['MACOS_NOTARY_SECRET'] = expansions.get("macos_notarization_secret", "")
print(' '.join(signing_cmd))
p = subprocess.Popen(signing_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=signing_env)

print(f"Signing tool completed with exitcode: {p.returncode}")
for line in iter(p.stdout.readline, b''):
    print(f'macnotary: {line.decode("utf-8").strip()}')
p.wait()

if p.returncode != 0:
    failed = True
    shutil.move(unsigned_archive, archive_name)
else:
    os.unlink(unsigned_archive)

if failed:
    exit(1)
