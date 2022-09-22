#! /bin/bash
sed -i '/^PubkeyAcceptedAlgorithms/s/$/,ssh-rsa/'  /etc/crypto-policies/back-ends/opensshserver.config
sed -i '/^HostKeyAlgorithms/s/$/,ssh-rsa/'  /etc/crypto-policies/back-ends/opensshserver.config
update-crypto-policies --set LEGACY
systemctl restart sshd

