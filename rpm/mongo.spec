Name: mongo-10gen
Conflicts: mongo, mongo-10gen-unstable
Obsoletes: mongo-stable
Version: 2.4.0
Release: mongodb_1%{?dist}
Summary: mongo client shell and tools
License: AGPL 3.0
URL: http://www.mongodb.org
Group: Applications/Databases

Source0: %{name}-%{version}.tar.gz
BuildRoot: %{_tmppath}/%{name}-%{version}-%{release}-root

%description
Mongo (from "huMONGOus") is a schema-free document-oriented database.
It features dynamic profileable queries, full indexing, replication
and fail-over support, efficient storage of large binary data objects,
and auto-sharding.

This package provides the mongo shell, import/export tools, and other
client utilities.

%package server
Summary: mongo server, sharding server, and support scripts
Group: Applications/Databases
Requires: mongo-10gen

%description server
Mongo (from "huMONGOus") is a schema-free document-oriented database.

This package provides the mongo server software, mongo sharding server
softwware, default configuration files, and init.d scripts.

%package devel
Summary: Headers and libraries for mongo development. 
Group: Applications/Databases

%description devel
Mongo (from "huMONGOus") is a schema-free document-oriented database.

This package provides the mongo static library and header files needed
to develop mongo client software.

%prep
%setup

%build
#scons --prefix=$RPM_BUILD_ROOT/usr all
# XXX really should have shared library here

%install
#scons --prefix=$RPM_BUILD_ROOT/usr install
mkdir -p $RPM_BUILD_ROOT/usr
cp -rv BINARIES/usr/bin $RPM_BUILD_ROOT/usr
mkdir -p $RPM_BUILD_ROOT/usr/share/man/man1
cp debian/*.1 $RPM_BUILD_ROOT/usr/share/man/man1/
# FIXME: remove this rm when mongosniff is back in the package
rm -v $RPM_BUILD_ROOT/usr/share/man/man1/mongosniff.1*
mkdir -p $RPM_BUILD_ROOT/etc/rc.d/init.d
cp -v rpm/init.d-mongod $RPM_BUILD_ROOT/etc/rc.d/init.d/mongod
chmod a+x $RPM_BUILD_ROOT/etc/rc.d/init.d/mongod
mkdir -p $RPM_BUILD_ROOT/etc
cp -v rpm/mongod.conf $RPM_BUILD_ROOT/etc/mongod.conf
mkdir -p $RPM_BUILD_ROOT/etc/sysconfig
cp -v rpm/mongod.sysconfig $RPM_BUILD_ROOT/etc/sysconfig/mongod
mkdir -p $RPM_BUILD_ROOT/var/lib/mongo
mkdir -p $RPM_BUILD_ROOT/var/log/mongo
touch $RPM_BUILD_ROOT/var/log/mongo/mongod.log

%clean
#scons -c
rm -rf $RPM_BUILD_ROOT

%pre server
if ! /usr/bin/id -g mongod &>/dev/null; then
    /usr/sbin/groupadd -r mongod
fi
if ! /usr/bin/id mongod &>/dev/null; then
    /usr/sbin/useradd -M -r -g mongod -d /var/lib/mongo -s /bin/false 	-c mongod mongod > /dev/null 2>&1
fi

%post server
if test $1 = 1
then
  /sbin/chkconfig --add mongod
fi

%preun server
if test $1 = 0
then
  /sbin/chkconfig --del mongod
fi

%postun server
if test $1 -ge 1
then
  /sbin/service mongod condrestart >/dev/null 2>&1 || :
fi

%files
%defattr(-,root,root,-)
#%doc README GNU-AGPL-3.0.txt

%{_bindir}/bsondump
%{_bindir}/mongo
%{_bindir}/mongodump
%{_bindir}/mongoexport
%{_bindir}/mongofiles
%{_bindir}/mongoimport
%{_bindir}/mongooplog
%{_bindir}/mongoperf
%{_bindir}/mongorestore
%{_bindir}/mongotop
%{_bindir}/mongostat
# FIXME: uncomment when mongosniff is back in the package
#%{_bindir}/mongosniff

# FIXME: uncomment this when there's a stable release whose source
# tree contains a bsondump man page.
%{_mandir}/man1/bsondump.1*
%{_mandir}/man1/mongo.1*
%{_mandir}/man1/mongodump.1*
%{_mandir}/man1/mongoexport.1*
%{_mandir}/man1/mongofiles.1*
%{_mandir}/man1/mongoimport.1*
%{_mandir}/man1/mongorestore.1*
%{_mandir}/man1/mongostat.1*
# FIXME: uncomment when mongosniff is back in the package
#%{_mandir}/man1/mongosniff.1*

%files server
%defattr(-,root,root,-)
%config(noreplace) /etc/mongod.conf
%{_bindir}/mongod
%{_bindir}/mongos
%{_mandir}/man1/mongod.1*
%{_mandir}/man1/mongos.1*
/etc/rc.d/init.d/mongod
/etc/sysconfig/mongod
#/etc/rc.d/init.d/mongos
%attr(0755,mongod,mongod) %dir /var/lib/mongo
%attr(0755,mongod,mongod) %dir /var/log/mongo
%attr(0640,mongod,mongod) %config(noreplace) %verify(not md5 size mtime) /var/log/mongo/mongod.log

%changelog
* Thu Jan 28 2010 Richard M Kreuter <richard@10gen.com>
- Minor fixes.

* Sat Oct 24 2009 Joe Miklojcik <jmiklojcik@shopwiki.com> - 
- Wrote mongo.spec.
