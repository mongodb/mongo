############################################################
# This section verifies start, stop, and restart.
# - stop bongod so that we begin testing from a stopped state
# - verify start, stop, and restart
############################################################

# service is not in path for commands with sudo on suse
service = os[:name] == 'suse' ? '/sbin/service' : 'service'

describe command("#{service} bongod stop") do
  its('exit_status') { should eq 0 }
end

describe command("#{service} bongod start") do
  its('exit_status') { should eq 0 }
end

describe service('bongod') do
  it { should be_running }
end

describe command("#{service} bongod stop") do
  its('exit_status') { should eq 0 }
end

describe service('bongod') do
  it { should_not be_running }
end

describe command("#{service} bongod restart") do
  its('exit_status') { should eq 0 }
end

describe service('bongod') do
  it { should be_running }
end

# wait to make sure bongod is ready
describe command("/inspec_wait.sh") do
  its('exit_status') { should eq 0 }
end

############################################################
# This section verifies files, directories, and users
# - files and directories exist and have correct attributes
# - bongod user exists and has correct attributes
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
systemd = !(upstart || sysvinit)
rpm = if os[:name] == 'amazon' || os[:name] == 'redhat' || os[:name] == 'suse'
        true
      else
        false
      end
deb = !rpm

# these files should exist on all systems
%w(
  /etc/bongod.conf
  /usr/bin/bongod
  /var/log/bongodb/bongod.log
).each do |filename|
  describe file(filename) do
    it { should be_file }
  end
end

if sysvinit
  describe file('/etc/init.d/bongod') do
    it { should be_file }
    it { should be_executable }
  end
end

if systemd
  describe file('/lib/systemd/system/bongod.service') do
    it { should be_file }
  end
end

if rpm
  %w(
    /var/lib/bongo
    /var/run/bongodb
  ).each do |filename|
    describe file(filename) do
      it { should be_directory }
    end
  end

  describe user('bongod') do
    it { should exist }
    its('groups') { should include 'bongod' }
    its('home') { should eq '/var/lib/bongo' }
    its('shell') { should eq '/bin/false' }
  end
end

if deb
  describe file('/var/lib/bongodb') do
    it { should be_directory }
  end

  describe user('bongodb') do
    it { should exist }
    its('groups') { should include 'bongodb' }
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
ulimits_cmd = 'cat /proc/$(pgrep bongod)/limits'

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

describe command('bongo --eval "db.smoke.insert({answer: 42})"') do
  its('exit_status') { should eq 0 }
  its('stdout') { should match(/.+WriteResult\({ "nInserted" : 1 }\).+/m) }
end

# read a document from the db
describe command('bongo --eval "db.smoke.findOne()"') do
  its('exit_status') { should eq 0 }
  its('stdout') { should match(/.+"answer" : 42.+/m) }
end

############################################################
# This section verifies uninstall.
############################################################

if rpm
  describe command('rpm -e $(rpm -qa | grep "bongodb.*server" | awk \'{print $1}\')') do
    its('exit_status') { should eq 0 }
  end
elsif deb
  describe command('dpkg -r $(dpkg -l | grep "bongodb.*server" | awk \'{print $2}\')') do
    its('exit_status') { should eq 0 }
  end
end

# make sure we cleaned up
%w(
  /lib/systemd/system/bongod.service
  /usr/bin/bongod
).each do |filename|
  describe file(filename) do
    it { should_not exist }
  end
end
