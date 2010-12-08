# jstests/dur/basic1.sh
# test durability

rm -rf 1
rm -rf 2
mkdir 1
mkdir 2

#win tmp
cp ../../db/debug/mongod.exe ../../

../../mongod --dbpath 1 &

../../mongo --eval db.foo.insert({x:1})

killall mongod

../../mongod --dbpath 2 &

../../mongo --eval db.foo.insert({x:1})

killall -9 mongod

# files should exist in 2/journal/ if this test script works

rm 2/test*

# recover
../../mongod --dbpath 2 --durTrace 4

diff 1/test.ns 2/test.ns
diff 1/test.1 2/test.1
