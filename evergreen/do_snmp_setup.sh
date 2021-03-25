set -o errexit
set -o verbose

cd src
mkdir -p snmpconf
cp -f src/mongo/db/modules/enterprise/docs/mongod.conf.master snmpconf/mongod.conf
