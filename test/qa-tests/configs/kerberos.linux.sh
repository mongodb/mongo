echo "107.23.89.149 kdc.10gen.me" | sudo tee -a /etc/hosts
sudo hostname "kdc.10gen.me"
sudo cp legacy/jstests/libs/mockkrb5.conf /etc/krb5.conf
kinit -p mockuser@10GEN.ME -k -t legacy/jstests/libs/mockuser.keytab
