#!/bin/sh

prev=0.16
release=0.17

# ... clone json-c, abi-compliance-checker, abi-dumper

mkdir build
cd build
CFLAGS=-Og cmake -DCMAKE_INSTALL_PREFIX=~/json-c-installs/json-c-${release} ..
make && make test && make install

# Assume the old version has already been built

cd ~/abi-compliance-checker
mkxml()
{
	ver="$1"
cat <<EOF > json-c-${ver}.xml
<foo>
<version>
   ${ver}
</version>

<headers>
../json-c-installs/json-c-${ver}/include/json-c
</headers>

<libs>
../json-c-installs/json-c-${ver}/lib64/libjson-c.so
</libs>
</foo>
EOF
}
mkxml ${release}
mkxml ${prev}

perl abi-compliance-checker.pl -lib json-c -dump json-c-${prev}.xml -dump-path ./ABI-${prev}.dump
perl abi-compliance-checker.pl -lib json-c -dump json-c-${release}.xml -dump-path ./ABI-${release}.dump
perl abi-compliance-checker.pl -l json-c -old ABI-${prev}.dump -new ABI-${release}.dump

echo "look in compat_reports/json-c/..."
