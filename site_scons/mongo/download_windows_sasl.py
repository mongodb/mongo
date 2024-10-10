import hashlib
import os
import time
import urllib
import urllib.request
import zipfile

SASL_HASH = "3e22e2b16f802277123590f64dfda44f1c9c8a2b7e758180cd956d8ab0965817"
SASL_URL = "https://s3.amazonaws.com/boxes.10gen.com/build/windows_cyrus_sasl-2.1.28.zip"


def hash_sasl(sasl_dir):
    md5_hash = hashlib.md5()
    for root, _, files in os.walk(sasl_dir):
        for name in files:
            if name.endswith("md5sum"):
                continue
            with open(os.path.join(root, name), "rb") as f:
                for block in iter(lambda: f.read(4096), b""):
                    md5_hash.update(block)
    return md5_hash.hexdigest()


def hash_sasl_zip(sasl_zip):
    sha_hash = hashlib.sha256()
    with open(sasl_zip, "rb") as f:
        for block in iter(lambda: f.read(4096), b""):
            sha_hash.update(block)
    return sha_hash.hexdigest()


def download_sasl(env):
    complete = False
    sasl_dir = env.Dir("$BUILD_ROOT/sasl_2_1_28").path
    sasl_md5 = os.path.join(sasl_dir, "sasl.md5sum")
    os.makedirs(sasl_dir, exist_ok=True)
    if os.path.exists(sasl_md5):
        with open(sasl_md5) as md5_file:
            if hash_sasl(sasl_dir) == md5_file.read():
                complete = True

    if not complete:
        print(f"Downloading sasl {SASL_URL}...")
        for i in range(1, 5):
            try:
                local_filename, _ = urllib.request.urlretrieve(SASL_URL)
                downloaded_hash = hash_sasl_zip(local_filename)
                if downloaded_hash != SASL_HASH:
                    raise urllib.error.URLError(
                        f"Downloaded file hash: {downloaded_hash} does not match expected hash: {SASL_HASH}"
                    )
            except urllib.error.URLError as exc:
                wait_time = i * i * 10
                if i == 4:
                    raise exc
                else:
                    print(f"Failed to download {SASL_URL} because of:\n{exc}")
                    print(f"Retrying in {wait_time}...")
                time.sleep(wait_time)

        zip_file_object = zipfile.ZipFile(local_filename, "r")
        zip_file_object.extractall(sasl_dir)
        zip_file_object.close()
        os.remove(local_filename)

        with open(sasl_md5, "w") as md5_file:
            md5_file.write(hash_sasl(sasl_dir))

    env.Append(CPPPATH=[f"#{sasl_dir}/include"])
    env.Append(LIBPATH=[f"#{sasl_dir}/lib"])
