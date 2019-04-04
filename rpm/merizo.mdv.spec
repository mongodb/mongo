%define name    merizodb
%define version %{dynamic_version}
%define release %{dynamic_release}

Name:    %{name}
Version: %{version}
Release: %{release}
Summary: MerizoDB client shell and tools
License: AGPL 3.0
URL: http://www.merizodb.org
Group: Databases

Source0: http://downloads.merizodb.org/src/%{name}-src-r%{version}.tar.gz
BuildRoot: %{_tmppath}/%{name}-%{version}-%{release}-root
BuildRequires: js-devel, readline-devel, boost-devel, pcre-devel
BuildRequires: gcc-c++, scons

%description
MerizoDB is built for scalability, performance and high availability, scaling from single server deployments to large, complex multi-site architectures. By leveraging in-memory computing, MerizoDB provides high performance for both reads and writes. MerizoDB’s native replication and automated failover enable enterprise-grade reliability and operational flexibility.

MerizoDB is an open-source database used by companies of all sizes, across all industries and for a wide variety of applications. It is an agile database that allows schemas to change quickly as applications evolve, while still providing the functionality developers expect from traditional databases, such as secondary indexes, a full query language and strict consistency.

MerizoDB has a rich client ecosystem including hadoop integration, officially supported drivers for 10 programming languages and environments, as well as 40 drivers supported by the user community.

MerizoDB features:
* JSON Data Model with Dynamic Schemas
* Auto-Sharding for Horizontal Scalability
* Built-In Replication for High Availability
* Rich Secondary Indexes, including geospatial
* TTL indexes
* Text Search
* Aggregation Framework & Native MapReduce

This package contains the merizo shell, import/export tools, and other client utilities.


%package server
Summary: MerizoDB server, sharding server, and support scripts
Group: Databases
Requires: merizodb

%description server
MerizoDB is built for scalability, performance and high availability, scaling from single server deployments to large, complex multi-site architectures. By leveraging in-memory computing, MerizoDB provides high performance for both reads and writes. MerizoDB’s native replication and automated failover enable enterprise-grade reliability and operational flexibility.

MerizoDB is an open-source database used by companies of all sizes, across all industries and for a wide variety of applications. It is an agile database that allows schemas to change quickly as applications evolve, while still providing the functionality developers expect from traditional databases, such as secondary indexes, a full query language and strict consistency.

MerizoDB has a rich client ecosystem including hadoop integration, officially supported drivers for 10 programming languages and environments, as well as 40 drivers supported by the user community.

MerizoDB features:
* JSON Data Model with Dynamic Schemas
* Auto-Sharding for Horizontal Scalability
* Built-In Replication for High Availability
* Rich Secondary Indexes, including geospatial
* TTL indexes
* Text Search
* Aggregation Framework & Native MapReduce

This package contains the MerizoDB server software, MerizoDB sharded cluster query router, default configuration files, and init.d scripts.


%package devel
Summary: Headers and libraries for MerizoDB development
Group: Databases

%description devel
MerizoDB is built for scalability, performance and high availability, scaling from single server deployments to large, complex multi-site architectures. By leveraging in-memory computing, MerizoDB provides high performance for both reads and writes. MerizoDB’s native replication and automated failover enable enterprise-grade reliability and operational flexibility.

MerizoDB is an open-source database used by companies of all sizes, across all industries and for a wide variety of applications. It is an agile database that allows schemas to change quickly as applications evolve, while still providing the functionality developers expect from traditional databases, such as secondary indexes, a full query language and strict consistency.

MerizoDB has a rich client ecosystem including hadoop integration, officially supported drivers for 10 programming languages and environments, as well as 40 drivers supported by the user community.

MerizoDB features:
* JSON Data Model with Dynamic Schemas
* Auto-Sharding for Horizontal Scalability
* Built-In Replication for High Availability
* Rich Secondary Indexes, including geospatial
* TTL indexes
* Text Search
* Aggregation Framework & Native MapReduce

This package provides the MerizoDB static library and header files needed to develop MerizoDB client software.

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
cp rpm/init.d-merizod $RPM_BUILD_ROOT%{_sysconfdir}/init.d/merizod
chmod a+x $RPM_BUILD_ROOT%{_sysconfdir}/init.d/merizod
mkdir -p $RPM_BUILD_ROOT%{_sysconfdir}
cp rpm/merizod.conf $RPM_BUILD_ROOT%{_sysconfdir}/merizod.conf
mkdir -p $RPM_BUILD_ROOT%{_sysconfdir}/sysconfig
cp rpm/merizod.sysconfig $RPM_BUILD_ROOT%{_sysconfdir}/sysconfig/merizod
mkdir -p $RPM_BUILD_ROOT%{_var}/lib/merizo
mkdir -p $RPM_BUILD_ROOT%{_var}/log/merizo
touch $RPM_BUILD_ROOT%{_var}/log/merizo/merizod.log

%clean
scons -c
rm -rf $RPM_BUILD_ROOT

%pre server
%{_sbindir}/useradd -M -r -U -d %{_var}/lib/merizo -s /bin/false \
    -c merizod merizod > /dev/null 2>&1

%post server
if test $1 = 1
then
  /sbin/chkconfig --add merizod
fi

%preun server
if test $1 = 0
then
  /sbin/chkconfig --del merizod
fi

%postun server
if test $1 -ge 1
then
  /sbin/service merizod stop >/dev/null 2>&1 || :
fi

%files
%defattr(-,root,root,-)
%doc README

%{_bindir}/install_compass
%{_bindir}/merizo
%{_bindir}/merizodump
%{_bindir}/merizoexport
%{_bindir}/merizofiles
%{_bindir}/merizoimport
%{_bindir}/merizorestore
%{_bindir}/merizostat

%{_mandir}/man1/merizo.1*
%{_mandir}/man1/merizod.1*
%{_mandir}/man1/merizodump.1*
%{_mandir}/man1/merizoexport.1*
%{_mandir}/man1/merizofiles.1*
%{_mandir}/man1/merizoimport.1*
%{_mandir}/man1/merizostat.1*
%{_mandir}/man1/merizorestore.1*

%files server
%defattr(-,root,root,-)
%config(noreplace) %{_sysconfdir}/merizod.conf
%{_bindir}/merizod
%{_bindir}/merizos
%{_mandir}/man1/merizos.1*
%{_sysconfdir}/init.d/merizod
%{_sysconfdir}/sysconfig/merizod
%attr(0755,merizod,merizod) %dir %{_var}/lib/merizo
%attr(0755,merizod,merizod) %dir %{_var}/log/merizo
%attr(0640,merizod,merizod) %config(noreplace) %verify(not md5 size mtime) %{_var}/log/merizo/merizod.log

%files devel
%{_includedir}/merizo
%{_libdir}/libmerizoclient.a
#%{_libdir}/libmerizotestfiles.a

%changelog
* Sun Mar 21 2010 Ludovic Bellière <xrogaan@gmail.com>
- Update merizo.spec for mandriva packaging

* Thu Jan 28 2010 Richard M Kreuter <richard@10gen.com>
- Minor fixes.

* Sat Oct 24 2009 Joe Miklojcik <jmiklojcik@shopwiki.com> - 
- Wrote merizo.spec.
