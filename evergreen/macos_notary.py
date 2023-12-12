import os
import platform
import shutil
import urllib.request
import subprocess
import zipfile
import stat
import sys

if platform.system().lower() != 'darwin':
    print("Not a macos system, skipping macos signing.")
    sys.exit(0)

if len(sys.argv) < 2:
    print("Must provide at least 1 archive to sign.")
    sys.exit(1)

supported_archs = {
    'arm64': 'arm64',
    'x86_64': 'amd64'
}
arch = platform.uname().machine.lower()

if arch not in supported_archs:
    print(f"Unsupported platform uname arch: {arch}, must be {supported_archs.keys()}")
    sys.exit(1)

macnotary_name = f'darwin_{supported_archs[arch]}'

if os.environ['project'] == "mongodb-mongo-master-nightly":
    signing_type = 'notarizeAndSign'
else:
    signing_type = 'sign'

macnotary_url = f'https://macos-notary-1628249594.s3.amazonaws.com/releases/client/latest/{macnotary_name}.zip'
print(f'Fetching macnotary tool from: {macnotary_url}')
local_filename, headers = urllib.request.urlretrieve(macnotary_url, f'{macnotary_name}.zip')
with zipfile.ZipFile(f'{macnotary_name}.zip') as zipf:
    zipf.extractall()

st = os.stat(f'{macnotary_name}/macnotary')
os.chmod(f'{macnotary_name}/macnotary', st.st_mode | stat.S_IEXEC)

failed = False
archives = sys.argv[1:]

for archive in archives:
    archive_base, archive_ext = os.path.splitext(archive)
    unsigned_archive = f'{archive_base}_unsigned{archive_ext}'
    shutil.move(archive, unsigned_archive)

    signing_cmd = [
        f'./{macnotary_name}/macnotary',
        '-f', f'{unsigned_archive}',
        '-m', f'{signing_type}',
        '-u', 'https://dev.macos-notary.build.10gen.cc/api',
        '-k', 'server',
        '--entitlements', 'etc/macos_entitlements.xml',
        '--verify',
        '-b', 'server.mongodb.com',
        '-i', f'{os.environ["task_id"]}',
        '-c', f'{os.environ["project"]}',
        '-o', f'{archive}'
    ]

    signing_env = os.environ.copy()
    signing_env['MACOS_NOTARY_SECRET'] = os.environ["macos_notarization_secret"]
    print(' '.join(signing_cmd))
    p = subprocess.Popen(signing_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=signing_env)

    print(f"Signing tool completed with exitcode: {p.returncode}")
    for line in iter(p.stdout.readline, b''):
        print(f'macnotary: {line.decode("utf-8").strip()}')

    # TODO: BUILD-14595 remove timeout when codesign doesn't frequently hang on macos hosts
    timeout = 3600
    timed_out = False
    try:
        p.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        print(f"ERROR: failed to finish signing in timeout period of {timeout} seconds. This most likely is related to hung codesign, see issues underlying BUILD-14595.")
        timed_out = True
        pass

    if timed_out:
        shutil.move(unsigned_archive, archive)
    elif p.returncode != 0:
        failed = True
        shutil.move(unsigned_archive, archive)
    else:
        os.unlink(unsigned_archive)

if failed:
    exit(1)

