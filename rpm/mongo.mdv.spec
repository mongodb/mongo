%define name    mongodb
%define version 1.3.4
%define release %mkrel 1

Name:    %{name}
Version: %{version}
Release: %{release}
Summary: MongoDB client shell and tools
License: AGPL 3.0
URL: http://www.mongodb.org
Group: Databases

Source0: http://downloads.mongodb.org/src/%{name}-src-r%{version}.tar.gz
BuildRoot: %{_tmppath}/%{name}-%{version}-%{release}-root
BuildRequires: js-devel, readline-devel, boost-devel, pcre-devel
BuildRequires: gcc-c++, scons

%description
Mongo (from "huMONGOus") is a schema-free document-oriented database.
It features dynamic profileable queries, full indexing, replication
and fail-over support, efficient storage of large binary data objects,
and auto-sharding.

This package provides the mongo shell, import/export tools, and other
client utilities.

%package server
Summary: MongoDB server, sharding server, and support scripts
Group: Databases
Requires: mongodb

%description server
Mongo (from "huMONGOus") is a schema-free document-oriented database.

This package provides the mongo server software, mongo sharding server
softwware, default configuration files, and init.d scripts.

%package devel
Summary: Headers and libraries for mongo development. 
Group: Databases

%description devel
Mongo (from "huMONGOus") is a schema-free document-oriented database.

This package provides the mongo static library and header files needed
to develop mongo client software.

%prep
%setup -n %{name}-src-r%{version}

%build
scons --prefix=$RPM_BUILD_ROOT/usr all
# XXX really should have shared library here

%install
scons --prefix=$RPM_BUILD_ROOT%{_usr} install
mkdir -p $RPM_BUILD_ROOT%{_mandir}/man1
cp debian/*.1 $RPM_BUILD_ROOT%{_mandir}/man1/
mkdir -p $RPM_BUILD_ROOT%{_sysconfdir}/rc.d/init.d
cp rpm/init.d-mongod $RPM_BUILD_ROOT%{_sysconfdir}/rc.d/init.d/mongod
chmod a+x $RPM_BUILD_ROOT%{_sysconfdir}/rc.d/init.d/mongod
mkdir -p $RPM_BUILD_ROOT%{_sysconfdir}
cp rpm/mongod.conf $RPM_BUILD_ROOT%{_sysconfdir}/mongod.conf
mkdir -p $RPM_BUILD_ROOT%{_sysconfdir}/sysconfig
cp rpm/mongod.sysconfig $RPM_BUILD_ROOT%{_sysconfdir}/sysconfig/mongod
mkdir -p $RPM_BUILD_ROOT%{_var}/lib/mongo
mkdir -p $RPM_BUILD_ROOT%{_var}/log/mongo
touch $RPM_BUILD_ROOT%{_var}/log/mongo/mongod.log

%clean
scons -c
rm -rf $RPM_BUILD_ROOT

%pre server
%{_sbindir}/useradd -M -r -U -d %{_var}/lib/mongo -s /bin/false \
    -c mongod mongod > /dev/null 2>&1

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
  /sbin/service mongod stop >/dev/null 2>&1 || :
fi

%files
%defattr(-,root,root,-)
%doc README GNU-AGPL-3.0.txt

%{_bindir}/mongo
%{_bindir}/mongodump
%{_bindir}/mongoexport
%{_bindir}/mongofiles
%{_bindir}/mongoimport
%{_bindir}/mongorestore
%{_bindir}/mongostat

%{_mandir}/man1/mongo.1*
%{_mandir}/man1/mongod.1*
%{_mandir}/man1/mongodump.1*
%{_mandir}/man1/mongoexport.1*
%{_mandir}/man1/mongofiles.1*
%{_mandir}/man1/mongoimport.1*
%{_mandir}/man1/mongosniff.1*
%{_mandir}/man1/mongostat.1*
%{_mandir}/man1/mongorestore.1*

%files server
%defattr(-,root,root,-)
%config(noreplace) %{_sysconfdir}/mongod.conf
%{_bindir}/mongod
%{_bindir}/mongos
%{_mandir}/man1/mongos.1*
%{_sysconfdir}/rc.d/init.d/mongod
%{_sysconfdir}/sysconfig/mongod
%attr(0755,mongod,mongod) %dir %{_var}/lib/mongo
%attr(0755,mongod,mongod) %dir %{_var}/log/mongo
%attr(0640,mongod,mongod) %config(noreplace) %verify(not md5 size mtime) %{_var}/log/mongo/mongod.log

%files devel
%{_includedir}/mongo
%{_libdir}/libmongoclient.a
#%{_libdir}/libmongotestfiles.a

%changelog
* Sun Mar 21 2010 Ludovic Bellière <xrogaan@gmail.com>
- Update mongo.spec for mandriva packaging

* Thu Jan 28 2010 Richard M Kreuter <richard@10gen.com>
- Minor fixes.

* Sat Oct 24 2009 Joe Miklojcik <jmiklojcik@shopwiki.com> - 
- Wrote mongo.spec.