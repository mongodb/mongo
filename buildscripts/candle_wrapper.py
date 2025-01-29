import hashlib
import subprocess
import sys
import uuid

# Wrapper script to substitute GENERATE_UPGRADE_CODE as starlark can't create hashes
candle_args = sys.argv[1:]
generate_code_replace_text = "GENERATE_UPGRADE_CODE"

# If our args contain GENERATE_UPGRADE_CODE we need to generate it, otherwise we can just call candle
if any(generate_code_replace_text in x for x in candle_args):
    # We must regenerate upgrade codes for each major release. These upgrade codes must also be
    # different for each MSI edition. To generate these we must be passed MongoDBMajorVersion and Edition
    # These are are things like -dMongoDBMajorVersion=8.1 and -dEdition=SSL
    for i, arg in enumerate(candle_args):
        if generate_code_replace_text in arg:
            upgrade_code_index = i
        if "MongoDBMajorVersion" in arg:
            major_version = arg.split("=")[1]
        if "Edition" in arg:
            msi_edition = arg.split("=")[1]

    # Create uuid for upgrade code
    m = hashlib.sha256()
    hash_str = "{}_{}".format(major_version, msi_edition)
    m.update(hash_str.encode())
    upgrade_code = str(uuid.UUID(bytes=m.digest()[0:16]))

    candle_args[upgrade_code_index] = candle_args[upgrade_code_index].replace(
        generate_code_replace_text, upgrade_code
    )

    # run candle with updated args
    subprocess.call(candle_args)
else:
    subprocess.call(candle_args)
