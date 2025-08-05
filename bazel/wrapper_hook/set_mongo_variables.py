import hashlib
import os
import pathlib
import platform
import subprocess

ARCH_NORMALIZE_MAP = {
    "amd64": "x86_64",
    "x86_64": "x86_64",
    "arm64": "aarch64",
    "aarch64": "aarch64",
    "ppc64le": "ppc64le",
    "s390x": "s390x",
}

def get_mongo_arch(args):
    arch = platform.machine().lower()
    if arch in ARCH_NORMALIZE_MAP:
        return ARCH_NORMALIZE_MAP[arch]
    else:
        return arch

def get_mongo_version(args):
    proc = subprocess.run(["git", "describe", "--abbrev=0"], capture_output=True, text=True)
    return proc.stdout.strip()[1:]

def write_mongo_variables_bazelrc(args):
    mongo_version = get_mongo_version(args)
    mongo_arch = get_mongo_arch(args)
    
    repo_root = pathlib.Path(os.path.abspath(__file__)).parent.parent.parent
    version_file = os.path.join(repo_root, ".bazelrc.mongo_variables")
    existing_hash = ""
    if os.path.exists(version_file):
        with open(version_file, encoding="utf-8") as f:
            existing_hash = hashlib.md5(f.read().encode()).hexdigest()

    bazelrc_contents = f"""
common --define=MONGO_ARCH={mongo_arch}
common --define=MONGO_VERSION={mongo_version}
"""
    current_hash = hashlib.md5(bazelrc_contents.encode()).hexdigest()
    if existing_hash != current_hash:
        with open(version_file, "w", encoding="utf-8") as f:
            f.write(bazelrc_contents)    
