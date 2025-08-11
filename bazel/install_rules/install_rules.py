import argparse
import json
import os
import shutil
import time

parser = argparse.ArgumentParser()

parser.add_argument("--depfile", action="append")
parser.add_argument("--install-dir")
parser.add_argument("--install-mode", choices=["copy", "symlink", "hardlink"], default="hardlink")

args = parser.parse_args()
if os.path.exists(args.install_dir):
    os.chmod(args.install_dir, 0o755)
    for root, dirs, files in os.walk(args.install_dir):
        for name in files:
            try:
                os.chmod(os.path.join(root, name), 0o755)
                os.unlink(os.path.join(root, name))
            # Sometimes we find files that don't exist
            # from os.walk - not sure why
            except FileNotFoundError:
                continue
        for name in dirs:
            try:
                os.chmod(os.path.join(root, name), 0o755)
            except FileNotFoundError:
                continue
    shutil.rmtree(args.install_dir)
os.makedirs(args.install_dir, exist_ok=True)


install_link = args.install_dir + "/../install"
os.makedirs(install_link, exist_ok=True)


def install(src, install_type):
    install_dst = os.path.join(args.install_dir, install_type, os.path.basename(src))
    link_dst = os.path.join(install_link, install_type, os.path.basename(src))
    if src.endswith(".dwp"):
        # Due to us creating our binaries using the _with_debug name
        # the dwp files also contain it. Strip the _with_debug from the name
        install_dst = install_dst.replace("_with_debug.dwp", ".dwp")
        link_dst = link_dst.replace("_with_debug.dwp", ".dwp")

    for dst in [install_dst, link_dst]:
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if args.install_mode == "hardlink":
            if os.path.exists(dst) and not os.path.samefile(src, dst):
                try:
                    if os.path.isdir(dst):
                        shutil.rmtree(dst)
                    else:
                        os.chmod(dst, 0o755)
                        os.unlink(dst)
                except FileNotFoundError as exc:
                    if link_dst == dst:
                        # if multiple installs are requested at once and happen
                        # to install the same file, it is ambiguous
                        # when one should be linked into the general install dir
                        # so we pass on exceptions
                        pass
                    else:
                        raise exc
                except OSError as exc:
                    if exc.strerror == "Directory not empty":
                        print("Encountered OSError: Directory not empty. Retrying...")
                        time.sleep(1)
                        shutil.rmtree(dst)
                    else:
                        raise exc
            if not os.path.exists(dst):
                try:
                    if os.path.isdir(src):
                        for root, _, files in os.walk(src):
                            for name in files:
                                dest_dir = os.path.dirname(os.path.join(root, name)).replace(
                                    src, dst
                                )
                                if not os.path.exists(dest_dir):
                                    os.makedirs(dest_dir)
                                try:
                                    os.link(os.path.join(root, name), os.path.join(dest_dir, name))
                                except OSError as exc:
                                    if exc.strerror == "Invalid argument":
                                        print("Encountered OSError: Invalid argument. Retrying...")
                                        time.sleep(1)
                                        os.link(os.path.join(root, name), os.path.join(dest_dir, name))
                    else:
                        try:
                            os.link(src, dst)
                        # If you try hardlinking across drives link will fail
                        except OSError:
                            try:
                                shutil.copy(src, dst)
                            except shutil.SameFileError:
                                pass
                except FileExistsError as exc:
                    if link_dst == dst:
                        # if multiple installs are requested at once and happen
                        # to install the same file, it is ambiguous
                        # when one should be linked into the general install dir
                        # so we pass on exceptions
                        pass
                    else:
                        raise exc
        else:
            raise Exception("Only hardlink mode is currently implemented.")

    return install_dst


for depfile in args.depfile:
    with open(depfile) as f:
        content = json.load(f)
        for binary in content["bins"]:
            install(binary, "bin")
        for lib in content["libs"]:
            install(lib, "lib")
        for file, folder in content["roots"].items():
            install(file, folder)
