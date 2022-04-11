# This Chef task installs MongoDB in a new EC2 instance spun up by Kitchen in
# preparation for running some basic server functionality tests.

artifacts_tarball = 'artifacts.tgz'
user = node['current_user']
homedir = node['etc']['passwd'][user]['dir']

ruby_block 'allow sudo over tty' do
  block do
    file = Chef::Util::FileEdit.new('/etc/sudoers')
    file.search_file_replace_line(/Defaults\s+requiretty/, '#Defaults requiretty')
    file.search_file_replace_line(/Defaults\s+requiretty/, '#Defaults !visiblepw')
    file.write_file
  end
end

# This file limits processes to 1024. It therefore interfereres with `ulimit -u` when present.
if platform_family? 'rhel'
  file '/etc/security/limits.d/90-nproc.conf' do
    action :delete
  end
end

if %w(6 7).include?(node['platform_version'][0]) and node['platform'] == 'redhat'

  # RHEL 6 client
  if node['platform'] == 'redhat' and node['platform_version'][0] == "6"
    rhui_client_url = "http://boxes.10gen.com/build/rh-amazon-rhui-client-els-3.0.45-1.el6.noarch.rpm"
  end

  # RHEL 7 client
  if node['platform'] == 'redhat' and node['platform_version'][0] == "7"
    rhui_client_url = "http://boxes.10gen.com/build/rh-amazon-rhui-client-3.0.45-1.el7.noarch.rpm"
  end

  execute 'remove old amazon rhui client' do
    command 'rpm -ev rh-amazon-rhui-client'
    live_stream true
  end
  execute 'install updated amazon rhui client' do
    command "rpm -ivh --nodeps #{rhui_client_url}"
    cwd homedir
    live_stream true
  end
  execute 'reinstall updated amazon rhui client for dependencies' do
    command 'yum reinstall -y rh-amazon-rhui-client*'
    live_stream true
  end
end

remote_file "#{homedir}/#{artifacts_tarball}" do
  source node['artifacts_url']
end

execute 'extract artifacts' do
  command "tar xzvf #{artifacts_tarball}"
  live_stream true
  cwd homedir
end

if platform_family? 'debian'

  # SERVER-40491 Debian 8 sources.list need to point to archive url
  if node['platform'] == 'debian' and node['platform_version'] == '8.1'
    cookbook_file '/etc/apt/sources.list' do
      source 'sources.list.debian8'
      owner 'root'
      group 'root'
      mode '0644'
      action :create
    end
  end

  execute 'apt update' do
    command 'apt update'
    live_stream true
  end

  ENV['DEBIAN_FRONTEND'] = 'noninteractive'
  package 'openssl'

  if node['platform_version'] == '16.04'
    execute 'update ca-certificates' do
      command 'apt-get install -y ca-certificates'
      live_stream true
    end
  end

  # the ubuntu 16.04 image does not have some dependencies installed by default
  # and it is required for the install_compass script
  execute 'install dependencies' do
    command 'apt-get install -y python libsasl2-modules-gssapi-mit'
    live_stream true
  end

  # dpkg returns 1 if dependencies are not satisfied, which they will not be
  # for enterprise builds. We install dependencies in the next block.
  execute 'install mongod' do
    command 'dpkg -i `find . -name "*server*.deb"`'
    live_stream true
    cwd homedir
    returns [0, 1]
  end

  # install the tools so we can test install_compass
  execute 'install mongo tools' do
    command 'dpkg -i `find . -name "*tools*.deb"`'
    live_stream true
    cwd homedir
    returns [0, 1]
  end

  # yum and zypper fetch dependencies automatically, but dpkg does not.
  # Installing the dependencies explicitly is fragile, so we reply on apt-get
  # to install dependencies after the fact.
  execute 'update and fix broken dependencies' do
    command 'apt update && apt -y -f install'
    live_stream true
  end

  execute 'install mongo shell' do
    command 'dpkg -i `find . -name "*shell*.deb"`'
    live_stream true
    cwd homedir
  end
end

if platform_family? 'rhel'
  bash 'wait for yum updates if they are running' do
    sleep 120
  end
  execute 'install mongod' do
    command 'yum install -y `find . -name "*server*.rpm"`'
    live_stream true
    cwd homedir
  end

  # install the tools so we can test install_compass
  execute 'install mongo tools' do
    command 'yum install -y `find . -name "*tools*.rpm"`'
    live_stream true
    cwd homedir
  end

  execute 'install mongo shell' do
    command 'yum install -y `find . -name "*shell*.rpm"`'
    live_stream true
    cwd homedir
  end
end

if platform_family? 'suse'
  bash 'wait for zypper lock to be released' do
    code <<-EOD
    retry_counter=0
    # We also need to make sure another instance of zypper isn't running while
    # we do our install, so just run zypper refresh until it doesn't fail.
    # Waiting for 2 minutes is copied from an internal project where we do this.
    until [ "$retry_counter" -ge "12" ]; do
        zypper refresh && exit 0
        retry_counter=$(($retry_counter + 1))
        [ "$retry_counter" = "12" ] && break
        sleep 10
    done
    exit 1
  EOD
  flags "-x"
  end

  execute 'install mongod' do
    command 'zypper --no-gpg-checks -n install `find . -name "*server*.rpm"`'
    live_stream true
    cwd homedir
  end

  execute 'install mongo tools' do
    command 'zypper --no-gpg-checks -n install `find . -name "*tools*.rpm"`'
    live_stream true
    cwd homedir
  end

  execute 'install mongo' do
    command 'zypper --no-gpg-checks -n install `find . -name "*shell*.rpm"`'
    live_stream true
    cwd homedir
  end
end

inspec_wait = <<HEREDOC
#!/bin/bash -x
ulimit -v unlimited
for i in {1..60}
do
  mongo --eval "db.smoke.insert({answer: 42})"
  if [ $? -eq 0 ]
  then
    exit 0
  else
    echo "sleeping"
    sleep 1
  fi
done
exit 1
HEREDOC

file '/inspec_wait.sh' do
  content inspec_wait
  mode '0755'
end
