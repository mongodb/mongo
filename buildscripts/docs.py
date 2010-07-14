"""Build the C++ client docs and the MongoDB server docs.
"""

from __future__ import with_statement
import os
import shutil
import subprocess

import markdown


def clean_dir(dir):
    try:
        shutil.rmtree(dir)
    except:
        pass
    os.makedirs(dir)


def convert_dir(source, dest):
    clean_dir(dest)

    for x in os.listdir(source + "/"):
        if not x.endswith(".md"):
            continue

        with open("%s/%s" % (source, x)) as f:
            raw = f.read()

        html = markdown.markdown(raw)
        print(x)

        with open("%s/%s" % (dest, x.replace(".md", ".html")), 'w') as o:
            o.write(html)


def gen_cplusplus(dir):
    clean_dir(dir)
    clean_dir("docs/doxygen")

    # Too noisy...
    with open("/dev/null") as null:
        subprocess.call(["doxygen", "doxygenConfig"], stdout=null, stderr=null)

    os.rename("docs/doxygen/html", dir)


def version():
    """Get the server version from doxygenConfig.
    """
    with open("doxygenConfig") as f:
        for line in f.readlines():
            if line.startswith("PROJECT_NUMBER"):
                return line.split("=")[1].strip()


def main():
    v = version()
    print("Generating server docs in docs/html/internal/%s" % v)
    convert_dir("docs", "docs/html/internal/%s" % v)
    print("Generating C++ docs in docs/html/cplusplus/%s" % v)
    gen_cplusplus("docs/html/cplusplus/%s" % v)


if __name__ == "__main__":
    main()


