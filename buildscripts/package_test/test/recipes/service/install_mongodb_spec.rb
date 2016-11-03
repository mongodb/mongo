############################################################
# This section verifies start, stop, and restart.
# - stop mongod so that we begin testing from a stopped state
# - verify start, stop, and restart
############################################################

# service is not in path for commands with sudo on suse
service = os[:name] == 'suse' ? '/sbin/service' : 'service'

describe command("#{service} mongod stop") do
  its('exit_status') { should eq 0 }
end

describe command("#{service} mongod start") do
  its('exit_status') { should eq 0 }
end

describe service('mongod') do
  it { should be_running }
end

describe command("#{service} mongod stop") do
  its('exit_status') { should eq 0 }
end

describe service('mongod') do
  it { should_not be_running }
end

describe command("#{service} mongod restart") do
  its('exit_status') { should eq 0 }
end

describe service('mongod') do
  it { should be_running }
end

# wait to make sure mongod is ready
describe command("/inspec_wait.sh") do
  its('exit_status') { should eq 0 }
end

############################################################
# This section verifies files, directories, and users
# - files and directories exist and have correct attributes
# - mongod user exists and has correct attributes
############################################################

# convenience variables for init system and package type
upstart = (os[:name] == 'ubuntu' && os[:release][0..1] == '14') ||
          (os[:name] == 'amazon')
sysvinit = if (os[:name] == 'debian' && os[:release][0] == '7') ||
              (os[:name] == 'redhat' && os[:release][0] == '6') ||
              (os[:name] == 'suse' && os[:release][0..1] == '11') ||
              (os[:name] == 'ubuntu' && os[:release][0..1] == '12')
             true
           else
             false
           end
rpm = if os[:name] == 'amazon' || os[:name] == 'redhat' || os[:name] == 'suse'
        true
      else
        false
      end
deb = !rpm

# these files should exist on all systems
%w(
  /etc/mongod.conf
  /usr/bin/mongod
  /var/log/mongodb/mongod.log
).each do |filename|
  describe file(filename) do
    it { should be_file }
  end
end

if sysvinit
  describe file('/etc/init.d/mongod') do
    it { should be_file }
    it { should be_executable }
  end
end

if os[:name] == 'ubuntu' && os[:release][0..1] == '16'
  describe file('/lib/systemd/system/mongod.service') do
    it { should be_file }
  end
end

if rpm
  %w(
    /var/lib/mongo
    /var/run/mongodb
  ).each do |filename|
    describe file(filename) do
      it { should be_directory }
    end
  end

  describe user('mongod') do
    it { should exist }
    its('groups') { should include 'mongod' }
    its('home') { should eq '/var/lib/mongo' }
    its('shell') { should eq '/bin/false' }
  end
end

if deb
  describe file('/var/lib/mongodb') do
    it { should be_directory }
  end

  describe user('mongodb') do
    it { should exist }
    its('groups') { should include 'mongodb' }
    its('shell') { should eq '/bin/false' }
  end
end

############################################################
# This section verifies ulimits.
############################################################

ulimits = {
  'Max file size'     => 'unlimited',
  'Max cpu time'      => 'unlimited',
  'Max address space' => 'unlimited',
  'Max open files'    => '64000',
  'Max resident set'  => 'unlimited',
  'Max processes'     => '64000'
}
ulimits_cmd = 'cat /proc/$(pgrep mongod)/limits'

ulimits.each do |limit, value|
  describe command("#{ulimits_cmd} | grep \"#{limit}\"") do
    its('stdout') { should match(/#{limit}\s+#{value}/) }
  end
end

############################################################
# This section verifies reads and writes.
# - insert a document into the database
# - verify that findOne() returns a matching document
############################################################

describe command('mongo --eval "db.smoke.insert({answer: 42})"') do
  its('exit_status') { should eq 0 }
  its('stdout') { should match(/.+WriteResult\({ "nInserted" : 1 }\).+/m) }
end

# read a document from the db
describe command('mongo --eval "db.smoke.findOne()"') do
  its('exit_status') { should eq 0 }
  its('stdout') { should match(/.+"answer" : 42.+/m) }
end

############################################################
# This section verifies uninstall.
############################################################

if rpm
  describe command('rpm -e $(rpm -qa | grep "mongodb.*server" | awk \'{print $1}\')') do
    its('exit_status') { should eq 0 }
  end
elsif deb
  describe command('dpkg -r $(dpkg -l | grep "mongodb.*server" | awk \'{print $2}\')') do
    its('exit_status') { should eq 0 }
  end
end

# make sure we cleaned up
%w(
  /lib/systemd/system/mongod.service
  /usr/bin/mongod
).each do |filename|
  describe file(filename) do
    it { should_not exist }
  end
end
