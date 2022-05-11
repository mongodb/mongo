set -o errexit
set -o verbose

cd src
mkdir -p snmpconf
cp -f src/mongo/db/modules/enterprise/docs/snmp/mongod.conf.master snmpconf/mongod.conf
