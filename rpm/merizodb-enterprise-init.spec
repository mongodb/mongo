Name: merizodb-enterprise
Prefix: /usr
Conflicts: merizo-10gen, merizo-10gen-server, merizo-10gen-unstable, merizo-10gen-unstable-enterprise, merizo-10gen-unstable-enterprise-merizos, merizo-10gen-unstable-enterprise-server, merizo-10gen-unstable-enterprise-shell, merizo-10gen-unstable-enterprise-tools, merizo-10gen-unstable-merizos, merizo-10gen-unstable-server, merizo-10gen-unstable-shell, merizo-10gen-unstable-tools, merizo18-10gen, merizo18-10gen-server, merizo20-10gen, merizo20-10gen-server, merizodb, merizodb-server, merizodb-dev, merizodb-clients, merizodb-10gen, merizodb-10gen-enterprise, merizodb-10gen-unstable, merizodb-10gen-unstable-enterprise, merizodb-10gen-unstable-enterprise-merizos, merizodb-10gen-unstable-enterprise-server, merizodb-10gen-unstable-enterprise-shell, merizodb-10gen-unstable-enterprise-tools, merizodb-10gen-unstable-merizos, merizodb-10gen-unstable-server, merizodb-10gen-unstable-shell, merizodb-10gen-unstable-tools, merizodb-enterprise-unstable, merizodb-enterprise-unstable-merizos, merizodb-enterprise-unstable-server, merizodb-enterprise-unstable-shell, merizodb-enterprise-unstable-tools, merizodb-nightly, merizodb-org, merizodb-org-merizos, merizodb-org-server, merizodb-org-shell, merizodb-org-tools, merizodb-stable, merizodb18-10gen, merizodb20-10gen, merizodb-org-unstable, merizodb-org-unstable-merizos, merizodb-org-unstable-server, merizodb-org-unstable-shell, merizodb-org-unstable-tools
Obsoletes: merizodb-enterprise-unstable, merizo-enterprise-unstable, merizo-10gen-enterprise
Provides: merizo-10gen-enterprise
Version: %{dynamic_version}
Release: %{dynamic_release}%{?dist}
Summary: MerizoDB open source document-oriented database system (enterprise metapackage)
License: Commercial
URL: http://www.merizodb.org
Group: Applications/Databases
Requires: merizodb-enterprise-server = %{version}, merizodb-enterprise-shell = %{version}, merizodb-enterprise-merizos = %{version}, merizodb-enterprise-tools = %{version}

Source0: %{name}-%{version}.tar.gz
BuildRoot: %{_tmppath}/%{name}-%{version}-%{release}-root

%if 0%{?suse_version}
%define timezone_pkg timezone
%else
%define timezone_pkg tzdata
%endif

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

This metapackage will install the merizo shell, import/export tools, other client utilities, server software, default configuration, and init.d scripts.

%package server
Summary: MerizoDB database server (enterprise)
Group: Applications/Databases
Requires: openssl %{?el6:>= 1.0.1}, net-snmp, cyrus-sasl, cyrus-sasl-plain, cyrus-sasl-gssapi, %{timezone_pkg}
Conflicts: merizo-10gen, merizo-10gen-server, merizo-10gen-unstable, merizo-10gen-unstable-enterprise, merizo-10gen-unstable-enterprise-merizos, merizo-10gen-unstable-enterprise-server, merizo-10gen-unstable-enterprise-shell, merizo-10gen-unstable-enterprise-tools, merizo-10gen-unstable-merizos, merizo-10gen-unstable-server, merizo-10gen-unstable-shell, merizo-10gen-unstable-tools, merizo18-10gen, merizo18-10gen-server, merizo20-10gen, merizo20-10gen-server, merizodb, merizodb-server, merizodb-dev, merizodb-clients, merizodb-10gen, merizodb-10gen-enterprise, merizodb-10gen-unstable, merizodb-10gen-unstable-enterprise, merizodb-10gen-unstable-enterprise-merizos, merizodb-10gen-unstable-enterprise-server, merizodb-10gen-unstable-enterprise-shell, merizodb-10gen-unstable-enterprise-tools, merizodb-10gen-unstable-merizos, merizodb-10gen-unstable-server, merizodb-10gen-unstable-shell, merizodb-10gen-unstable-tools, merizodb-enterprise-unstable, merizodb-enterprise-unstable-merizos, merizodb-enterprise-unstable-server, merizodb-enterprise-unstable-shell, merizodb-enterprise-unstable-tools, merizodb-nightly, merizodb-org, merizodb-org-merizos, merizodb-org-server, merizodb-org-shell, merizodb-org-tools, merizodb-stable, merizodb18-10gen, merizodb20-10gen, merizodb-org-unstable, merizodb-org-unstable-merizos, merizodb-org-unstable-server, merizodb-org-unstable-shell, merizodb-org-unstable-tools
Obsoletes: merizo-10gen-enterprise-server
Provides: merizo-10gen-enterprise-server

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

This package contains the MerizoDB server software, default configuration files, and init.d scripts.

%package shell
Summary: MerizoDB shell client (enterprise)
Group: Applications/Databases
Requires: openssl %{?el6:>= 1.0.1}, cyrus-sasl, cyrus-sasl-plain, cyrus-sasl-gssapi
Conflicts: merizo-10gen, merizo-10gen-server, merizo-10gen-unstable, merizo-10gen-unstable-enterprise, merizo-10gen-unstable-enterprise-merizos, merizo-10gen-unstable-enterprise-server, merizo-10gen-unstable-enterprise-shell, merizo-10gen-unstable-enterprise-tools, merizo-10gen-unstable-merizos, merizo-10gen-unstable-server, merizo-10gen-unstable-shell, merizo-10gen-unstable-tools, merizo18-10gen, merizo18-10gen-server, merizo20-10gen, merizo20-10gen-server, merizodb, merizodb-server, merizodb-dev, merizodb-clients, merizodb-10gen, merizodb-10gen-enterprise, merizodb-10gen-unstable, merizodb-10gen-unstable-enterprise, merizodb-10gen-unstable-enterprise-merizos, merizodb-10gen-unstable-enterprise-server, merizodb-10gen-unstable-enterprise-shell, merizodb-10gen-unstable-enterprise-tools, merizodb-10gen-unstable-merizos, merizodb-10gen-unstable-server, merizodb-10gen-unstable-shell, merizodb-10gen-unstable-tools, merizodb-enterprise-unstable, merizodb-enterprise-unstable-merizos, merizodb-enterprise-unstable-server, merizodb-enterprise-unstable-shell, merizodb-enterprise-unstable-tools, merizodb-nightly, merizodb-org, merizodb-org-merizos, merizodb-org-server, merizodb-org-shell, merizodb-org-tools, merizodb-stable, merizodb18-10gen, merizodb20-10gen, merizodb-org-unstable, merizodb-org-unstable-merizos, merizodb-org-unstable-server, merizodb-org-unstable-shell, merizodb-org-unstable-tools
Obsoletes: merizo-10gen-enterprise-shell
Provides: merizo-10gen-enterprise-shell

%description shell
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

This package contains the merizo shell.

%package merizos
Summary: MerizoDB sharded cluster query router (enterprise)
Group: Applications/Databases
Requires: %{timezone_pkg}
Conflicts: merizo-10gen, merizo-10gen-server, merizo-10gen-unstable, merizo-10gen-unstable-enterprise, merizo-10gen-unstable-enterprise-merizos, merizo-10gen-unstable-enterprise-server, merizo-10gen-unstable-enterprise-shell, merizo-10gen-unstable-enterprise-tools, merizo-10gen-unstable-merizos, merizo-10gen-unstable-server, merizo-10gen-unstable-shell, merizo-10gen-unstable-tools, merizo18-10gen, merizo18-10gen-server, merizo20-10gen, merizo20-10gen-server, merizodb, merizodb-server, merizodb-dev, merizodb-clients, merizodb-10gen, merizodb-10gen-enterprise, merizodb-10gen-unstable, merizodb-10gen-unstable-enterprise, merizodb-10gen-unstable-enterprise-merizos, merizodb-10gen-unstable-enterprise-server, merizodb-10gen-unstable-enterprise-shell, merizodb-10gen-unstable-enterprise-tools, merizodb-10gen-unstable-merizos, merizodb-10gen-unstable-server, merizodb-10gen-unstable-shell, merizodb-10gen-unstable-tools, merizodb-enterprise-unstable, merizodb-enterprise-unstable-merizos, merizodb-enterprise-unstable-server, merizodb-enterprise-unstable-shell, merizodb-enterprise-unstable-tools, merizodb-nightly, merizodb-org, merizodb-org-merizos, merizodb-org-server, merizodb-org-shell, merizodb-org-tools, merizodb-stable, merizodb18-10gen, merizodb20-10gen, merizodb-org-unstable, merizodb-org-unstable-merizos, merizodb-org-unstable-server, merizodb-org-unstable-shell, merizodb-org-unstable-tools
Obsoletes: merizo-10gen-enterprise-merizos
Provides: merizo-10gen-enterprise-merizos

%description merizos
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

This package contains merizos, the MerizoDB sharded cluster query router.

%package tools
Summary: MerizoDB tools (enterprise)
Group: Applications/Databases
Requires: openssl %{?el6:>= 1.0.1}, cyrus-sasl, cyrus-sasl-plain, cyrus-sasl-gssapi
Conflicts: merizo-10gen, merizo-10gen-server, merizo-10gen-unstable, merizo-10gen-unstable-enterprise, merizo-10gen-unstable-enterprise-merizos, merizo-10gen-unstable-enterprise-server, merizo-10gen-unstable-enterprise-shell, merizo-10gen-unstable-enterprise-tools, merizo-10gen-unstable-merizos, merizo-10gen-unstable-server, merizo-10gen-unstable-shell, merizo-10gen-unstable-tools, merizo18-10gen, merizo18-10gen-server, merizo20-10gen, merizo20-10gen-server, merizodb, merizodb-server, merizodb-dev, merizodb-clients, merizodb-10gen, merizodb-10gen-enterprise, merizodb-10gen-unstable, merizodb-10gen-unstable-enterprise, merizodb-10gen-unstable-enterprise-merizos, merizodb-10gen-unstable-enterprise-server, merizodb-10gen-unstable-enterprise-shell, merizodb-10gen-unstable-enterprise-tools, merizodb-10gen-unstable-merizos, merizodb-10gen-unstable-server, merizodb-10gen-unstable-shell, merizodb-10gen-unstable-tools, merizodb-enterprise-unstable, merizodb-enterprise-unstable-merizos, merizodb-enterprise-unstable-server, merizodb-enterprise-unstable-shell, merizodb-enterprise-unstable-tools, merizodb-nightly, merizodb-org, merizodb-org-merizos, merizodb-org-server, merizodb-org-shell, merizodb-org-tools, merizodb-stable, merizodb18-10gen, merizodb20-10gen, merizodb-org-unstable, merizodb-org-unstable-merizos, merizodb-org-unstable-server, merizodb-org-unstable-shell, merizodb-org-unstable-tools
Obsoletes: merizo-10gen-enterprise-tools
Provides: merizo-10gen-enterprise-tools

%description tools
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

This package contains standard utilities for interacting with MerizoDB.

%package devel
Summary: Headers and libraries for MerizoDB development
Group: Applications/Databases
Conflicts: merizo-10gen, merizo-10gen-server, merizo-10gen-unstable, merizo-10gen-unstable-enterprise, merizo-10gen-unstable-enterprise-merizos, merizo-10gen-unstable-enterprise-server, merizo-10gen-unstable-enterprise-shell, merizo-10gen-unstable-enterprise-tools, merizo-10gen-unstable-merizos, merizo-10gen-unstable-server, merizo-10gen-unstable-shell, merizo-10gen-unstable-tools, merizo18-10gen, merizo18-10gen-server, merizo20-10gen, merizo20-10gen-server, merizodb, merizodb-server, merizodb-dev, merizodb-clients, merizodb-10gen, merizodb-10gen-enterprise, merizodb-10gen-unstable, merizodb-10gen-unstable-enterprise, merizodb-10gen-unstable-enterprise-merizos, merizodb-10gen-unstable-enterprise-server, merizodb-10gen-unstable-enterprise-shell, merizodb-10gen-unstable-enterprise-tools, merizodb-10gen-unstable-merizos, merizodb-10gen-unstable-server, merizodb-10gen-unstable-shell, merizodb-10gen-unstable-tools, merizodb-enterprise-unstable, merizodb-enterprise-unstable-merizos, merizodb-enterprise-unstable-server, merizodb-enterprise-unstable-shell, merizodb-enterprise-unstable-tools, merizodb-nightly, merizodb-org, merizodb-org-merizos, merizodb-org-server, merizodb-org-shell, merizodb-org-tools, merizodb-stable, merizodb18-10gen, merizodb20-10gen, merizodb-org-unstable, merizodb-org-unstable-merizos, merizodb-org-unstable-server, merizodb-org-unstable-shell, merizodb-org-unstable-tools
Obsoletes: merizo-10gen-enterprise-devel
Provides: merizo-10gen-enterprise-devel

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
%setup

%build

%install
mkdir -p $RPM_BUILD_ROOT/usr
cp -rv bin $RPM_BUILD_ROOT/usr
mkdir -p $RPM_BUILD_ROOT/usr/share/man/man1
cp debian/*.1 $RPM_BUILD_ROOT/usr/share/man/man1/
mkdir -p $RPM_BUILD_ROOT/etc/init.d
cp -v rpm/init.d-merizod $RPM_BUILD_ROOT/etc/init.d/merizod
chmod a+x $RPM_BUILD_ROOT/etc/init.d/merizod
mkdir -p $RPM_BUILD_ROOT/etc
cp -v rpm/merizod.conf $RPM_BUILD_ROOT/etc/merizod.conf
mkdir -p $RPM_BUILD_ROOT/etc/sysconfig
cp -v rpm/merizod.sysconfig $RPM_BUILD_ROOT/etc/sysconfig/merizod
mkdir -p $RPM_BUILD_ROOT/var/lib/merizo
mkdir -p $RPM_BUILD_ROOT/var/log/merizodb
mkdir -p $RPM_BUILD_ROOT/var/run/merizodb
touch $RPM_BUILD_ROOT/var/log/merizodb/merizod.log

%clean
rm -rf $RPM_BUILD_ROOT

%pre server
if ! /usr/bin/id -g merizod &>/dev/null; then
    /usr/sbin/groupadd -r merizod
fi
if ! /usr/bin/id merizod &>/dev/null; then
    /usr/sbin/useradd -M -r -g merizod -d /var/lib/merizo -s /bin/false   -c merizod merizod > /dev/null 2>&1
fi

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
  /sbin/service merizod condrestart >/dev/null 2>&1 || :
fi

%files

%files server
%defattr(-,root,root,-)
%config(noreplace) /etc/merizod.conf
%{_bindir}/merizod
%{_bindir}/merizocryptd
%{_mandir}/man1/merizod.1*
/etc/init.d/merizod
%config(noreplace) /etc/sysconfig/merizod
%attr(0755,merizod,merizod) %dir /var/lib/merizo
%attr(0755,merizod,merizod) %dir /var/log/merizodb
%attr(0755,merizod,merizod) %dir /var/run/merizodb
%attr(0640,merizod,merizod) %config(noreplace) %verify(not md5 size mtime) /var/log/merizodb/merizod.log
%doc snmp/MERIZOD-MIB.txt
%doc snmp/MERIZODBINC-MIB.txt
%doc snmp/merizod.conf.master
%doc snmp/merizod.conf.subagent
%doc snmp/README-snmp.txt
%doc LICENSE-Enterprise.txt
%doc README
%doc THIRD-PARTY-NOTICES
%doc MPL-2



%files shell
%defattr(-,root,root,-)
%{_bindir}/merizo
%{_mandir}/man1/merizo.1*

%files merizos
%defattr(-,root,root,-)
%{_bindir}/merizos
%{_mandir}/man1/merizos.1*

%files tools
%defattr(-,root,root,-)
#%doc README
%doc THIRD-PARTY-NOTICES.gotools

%{_bindir}/bsondump
%{_bindir}/install_compass
%{_bindir}/merizodecrypt
%{_bindir}/merizoldap
%{_bindir}/merizodump
%{_bindir}/merizoexport
%{_bindir}/merizofiles
%{_bindir}/merizoimport
%{_bindir}/merizorestore
%{_bindir}/merizotop
%{_bindir}/merizostat

%{_mandir}/man1/bsondump.1*
%{_mandir}/man1/merizodump.1*
%{_mandir}/man1/merizoexport.1*
%{_mandir}/man1/merizofiles.1*
%{_mandir}/man1/merizoimport.1*
%{_mandir}/man1/merizorestore.1*
%{_mandir}/man1/merizotop.1*
%{_mandir}/man1/merizostat.1*

%changelog
* Thu Dec 19 2013 Ernie Hershey <ernie.hershey@merizodb.com>
- Packaging file cleanup

* Thu Jan 28 2010 Richard M Kreuter <richard@10gen.com>
- Minor fixes.

* Sat Oct 24 2009 Joe Miklojcik <jmiklojcik@shopwiki.com> -
- Wrote merizo.spec.
