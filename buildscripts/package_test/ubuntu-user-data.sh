#! /bin/bash
echo "HostkeyAlgorithms +ssh-rsa" >> /etc/ssh/sshd_config
echo "PubkeyAcceptedAlgorithms +ssh-rsa" >> /etc/ssh/sshd_config

systemctl restart sshd

touch /root/userdata_ran
