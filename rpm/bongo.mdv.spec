%define name    bongodb
%define version %{dynamic_version}
%define release %{dynamic_release}

Name:    %{name}
Version: %{version}
Release: %{release}
Summary: BongoDB client shell and tools
License: AGPL 3.0
URL: http://www.bongodb.org
Group: Databases

Source0: http://downloads.bongodb.org/src/%{name}-src-r%{version}.tar.gz
BuildRoot: %{_tmppath}/%{name}-%{version}-%{release}-root
BuildRequires: js-devel, readline-devel, boost-devel, pcre-devel
BuildRequires: gcc-c++, scons

%description
BongoDB is built for scalability, performance and high availability, scaling from single server deployments to large, complex multi-site architectures. By leveraging in-memory computing, BongoDB provides high performance for both reads and writes. BongoDB’s native replication and automated failover enable enterprise-grade reliability and operational flexibility.

BongoDB is an open-source database used by companies of all sizes, across all industries and for a wide variety of applications. It is an agile database that allows schemas to change quickly as applications evolve, while still providing the functionality developers expect from traditional databases, such as secondary indexes, a full query language and strict consistency.

BongoDB has a rich client ecosystem including hadoop integration, officially supported drivers for 10 programming languages and environments, as well as 40 drivers supported by the user community.

BongoDB features:
* JSON Data Model with Dynamic Schemas
* Auto-Sharding for Horizontal Scalability
* Built-In Replication for High Availability
* Rich Secondary Indexes, including geospatial
* TTL indexes
* Text Search
* Aggregation Framework & Native MapReduce

This package contains the bongo shell, import/export tools, and other client utilities.


%package server
Summary: BongoDB server, sharding server, and support scripts
Group: Databases
Requires: bongodb

%description server
BongoDB is built for scalability, performance and high availability, scaling from single server deployments to large, complex multi-site architectures. By leveraging in-memory computing, BongoDB provides high performance for both reads and writes. BongoDB’s native replication and automated failover enable enterprise-grade reliability and operational flexibility.

BongoDB is an open-source database used by companies of all sizes, across all industries and for a wide variety of applications. It is an agile database that allows schemas to change quickly as applications evolve, while still providing the functionality developers expect from traditional databases, such as secondary indexes, a full query language and strict consistency.

BongoDB has a rich client ecosystem including hadoop integration, officially supported drivers for 10 programming languages and environments, as well as 40 drivers supported by the user community.

BongoDB features:
* JSON Data Model with Dynamic Schemas
* Auto-Sharding for Horizontal Scalability
* Built-In Replication for High Availability
* Rich Secondary Indexes, including geospatial
* TTL indexes
* Text Search
* Aggregation Framework & Native MapReduce

This package contains the BongoDB server software, BongoDB sharded cluster query router, default configuration files, and init.d scripts.


%package devel
Summary: Headers and libraries for BongoDB development
Group: Databases

%description devel
BongoDB is built for scalability, performance and high availability, scaling from single server deployments to large, complex multi-site architectures. By leveraging in-memory computing, BongoDB provides high performance for both reads and writes. BongoDB’s native replication and automated failover enable enterprise-grade reliability and operational flexibility.

BongoDB is an open-source database used by companies of all sizes, across all industries and for a wide variety of applications. It is an agile database that allows schemas to change quickly as applications evolve, while still providing the functionality developers expect from traditional databases, such as secondary indexes, a full query language and strict consistency.

BongoDB has a rich client ecosystem including hadoop integration, officially supported drivers for 10 programming languages and environments, as well as 40 drivers supported by the user community.

BongoDB features:
* JSON Data Model with Dynamic Schemas
* Auto-Sharding for Horizontal Scalability
* Built-In Replication for High Availability
* Rich Secondary Indexes, including geospatial
* TTL indexes
* Text Search
* Aggregation Framework & Native MapReduce

This package provides the BongoDB static library and header files needed to develop BongoDB client software.

%prep
%setup -n %{name}-src-r%{version}

%build
scons --prefix=$RPM_BUILD_ROOT/usr all
# XXX really should have shared library here

%install
scons --prefix=$RPM_BUILD_ROOT%{_usr} install
mkdir -p $RPM_BUILD_ROOT%{_mandir}/man1
cp debian/*.1 $RPM_BUILD_ROOT%{_mandir}/man1/
mkdir -p $RPM_BUILD_ROOT%{_sysconfdir}/init.d
cp rpm/init.d-bongod $RPM_BUILD_ROOT%{_sysconfdir}/init.d/bongod
chmod a+x $RPM_BUILD_ROOT%{_sysconfdir}/init.d/bongod
mkdir -p $RPM_BUILD_ROOT%{_sysconfdir}
cp rpm/bongod.conf $RPM_BUILD_ROOT%{_sysconfdir}/bongod.conf
mkdir -p $RPM_BUILD_ROOT%{_sysconfdir}/sysconfig
cp rpm/bongod.sysconfig $RPM_BUILD_ROOT%{_sysconfdir}/sysconfig/bongod
mkdir -p $RPM_BUILD_ROOT%{_var}/lib/bongo
mkdir -p $RPM_BUILD_ROOT%{_var}/log/bongo
touch $RPM_BUILD_ROOT%{_var}/log/bongo/bongod.log

%clean
scons -c
rm -rf $RPM_BUILD_ROOT

%pre server
%{_sbindir}/useradd -M -r -U -d %{_var}/lib/bongo -s /bin/false \
    -c bongod bongod > /dev/null 2>&1

%post server
if test $1 = 1
then
  /sbin/chkconfig --add bongod
fi

%preun server
if test $1 = 0
then
  /sbin/chkconfig --del bongod
fi

%postun server
if test $1 -ge 1
then
  /sbin/service bongod stop >/dev/null 2>&1 || :
fi

%files
%defattr(-,root,root,-)
%doc README GNU-AGPL-3.0.txt

%{_bindir}/bongo
%{_bindir}/bongodump
%{_bindir}/bongoexport
%{_bindir}/bongofiles
%{_bindir}/bongoimport
%{_bindir}/bongorestore
%{_bindir}/bongostat

%{_mandir}/man1/bongo.1*
%{_mandir}/man1/bongod.1*
%{_mandir}/man1/bongodump.1*
%{_mandir}/man1/bongoexport.1*
%{_mandir}/man1/bongofiles.1*
%{_mandir}/man1/bongoimport.1*
%{_mandir}/man1/bongosniff.1*
%{_mandir}/man1/bongostat.1*
%{_mandir}/man1/bongorestore.1*

%files server
%defattr(-,root,root,-)
%config(noreplace) %{_sysconfdir}/bongod.conf
%{_bindir}/bongod
%{_bindir}/bongos
%{_mandir}/man1/bongos.1*
%{_sysconfdir}/init.d/bongod
%{_sysconfdir}/sysconfig/bongod
%attr(0755,bongod,bongod) %dir %{_var}/lib/bongo
%attr(0755,bongod,bongod) %dir %{_var}/log/bongo
%attr(0640,bongod,bongod) %config(noreplace) %verify(not md5 size mtime) %{_var}/log/bongo/bongod.log

%files devel
%{_includedir}/bongo
%{_libdir}/libbongoclient.a
#%{_libdir}/libbongotestfiles.a

%changelog
* Sun Mar 21 2010 Ludovic Bellière <xrogaan@gmail.com>
- Update bongo.spec for mandriva packaging

* Thu Jan 28 2010 Richard M Kreuter <richard@10gen.com>
- Minor fixes.

* Sat Oct 24 2009 Joe Miklojcik <jmiklojcik@shopwiki.com> - 
- Wrote bongo.spec.
