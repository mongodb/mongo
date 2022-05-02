set -o errexit
set -o verbose

for i in $(seq 1 $repeat); do
  bash ./src/evergreen/scons_compile.sh
  cd src
  if [ $i == 1 ]; then
    mv build.ninja build.ninja.a
    mv build build.a
    mv scons_stdout.log scons_stdout.log.a
  else
    if ! diff build.ninja build.ninja.a > build.ninja.diff; then
      tar -cvf ninja.tgz build.ninja{,.a} build{,.a} build.ninja.diff scons_stdout.log{,.a}
      echo "1" > ninja_determinism.txt
      exit 0
    fi
    rm -rf build.ninja build build.ninja.diff scons_stdout.log
  fi
  cd ..
done

echo "0" > src/ninja_determinism.txt
