import argparse
import json
import os
import shutil

parser = argparse.ArgumentParser()
parser.add_argument("--depfile", action="append")
parser.add_argument("--install-dir")
parser.add_argument("--install-mode", choices=["copy", "symlink", "hardlink"], default="hardlink")

args = parser.parse_args()

if os.path.exists(args.install_dir):
    os.chmod(args.install_dir, 0o755)
    for root, dirs, files in os.walk(args.install_dir):
        for name in files:
            os.chmod(os.path.join(root, name), 0o755)
            os.unlink(os.path.join(root, name))
        for name in dirs:
            os.chmod(os.path.join(root, name), 0o755)
    shutil.rmtree(args.install_dir)
os.makedirs(args.install_dir, exist_ok=True)


install_link = args.install_dir + "/../install"
os.makedirs(install_link, exist_ok=True)


def install(src, install_type):
    install_dst = os.path.join(args.install_dir, install_type, os.path.basename(src))
    link_dst = os.path.join(install_link, install_type, os.path.basename(src))

    for dst in [install_dst, link_dst]:
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if args.install_mode == "hardlink":
            if os.path.exists(dst) and not os.path.samefile(src, dst):
                try:
                    if os.path.isdir(dst):
                        shutil.rmtree(dst)
                    else:
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
            if not os.path.exists(dst):
                try:
                    if os.path.isdir(src):
                        for root, _, files in os.walk(src):
                            for name in files:
                                dest_dir = os.path.dirname(os.path.join(root, name)).replace(
                                    src, dst
                                )
                                os.makedirs(dest_dir)
                                os.link(os.path.join(root, name), os.path.join(dest_dir, name))
                    else:
                        os.link(src, dst)
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
