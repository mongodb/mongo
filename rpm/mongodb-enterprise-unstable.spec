%if 0%{?suse_version}
%define _sharedstatedir %{_localstatedir}/lib
%endif

%if ! %{defined _docdir}
%define _docdir %{_datadir}/doc
%endif

%if ! %{defined _rundir}
%define _rundir %{_localstatedir}/run
%endif

%define _name mongodb-enterprise-unstable

Name: %{_name}-database
Prefix: /usr
Prefix: /var
Prefix: /etc
Conflicts: mongo-10gen, mongo-10gen-enterprise, mongo-10gen-enterprise-server, mongo-10gen-server, mongo-10gen-unstable, mongo-10gen-unstable-enterprise, mongo-10gen-unstable-enterprise-mongos, mongo-10gen-unstable-enterprise-server, mongo-10gen-unstable-enterprise-shell, mongo-10gen-unstable-enterprise-tools, mongo-10gen-unstable-mongos, mongo-10gen-unstable-server, mongo-10gen-unstable-shell, mongo-10gen-unstable-tools, mongo18-10gen, mongo18-10gen-server, mongo20-10gen, mongo20-10gen-server, mongodb, mongodb-server, mongodb-dev, mongodb-clients, mongodb-10gen, mongodb-10gen-enterprise, mongodb-10gen-unstable, mongodb-10gen-unstable-enterprise, mongodb-10gen-unstable-enterprise-mongos, mongodb-10gen-unstable-enterprise-server, mongodb-10gen-unstable-enterprise-shell, mongodb-10gen-unstable-enterprise-tools, mongodb-10gen-unstable-mongos, mongodb-10gen-unstable-server, mongodb-10gen-unstable-shell, mongodb-10gen-unstable-tools, mongodb-enterprise, mongodb-enterprise-mongos, mongodb-enterprise-server, mongodb-enterprise-shell, mongodb-enterprise-tools, mongodb-enterprise-cryptd, mongodb-nightly, mongodb-org, mongodb-org-mongos, mongodb-org-server, mongodb-org-shell, mongodb-org-tools, mongodb-stable, mongodb18-10gen, mongodb20-10gen, mongodb-org-unstable, mongodb-org-unstable-mongos, mongodb-org-unstable-server, mongodb-org-unstable-shell, mongodb-org-unstable-tools
Obsoletes: mongodb-enterprise-unstable,mongo-enterprise-unstable
Version: %{dynamic_version}
Release: %{dynamic_release}%{?dist}
Summary: MongoDB open source document-oriented database system (enterprise metapackage)
License: Commercial
URL: http://www.mongodb.org
Group: Applications/Databases
Requires: mongodb-enterprise-unstable-cryptd, mongodb-enterprise-unstable-mongos, mongodb-enterprise-unstable-server, mongodb-enterprise-unstable-database-tools-extra

%if 0%{?rhel} >= 8 || 0%{?fedora} >= 30
BuildRequires: /usr/bin/pathfix.py, python3-devel
%endif

Source0: %{_name}-%{version}.tar.gz
BuildRoot: %{_tmppath}/%{_name}-%{version}-%{release}-root

%if 0%{?rhel} >= 8 || 0%{?fedora} >= 30
%define python_pkg python3
%else
%define python_pkg python2
%endif

%if 0%{?suse_version}
%define timezone_pkg timezone
%define python_pkg python
%else
%define timezone_pkg tzdata
%endif

%description
MongoDB is built for scalability, performance and high availability, scaling from single server deployments to large, complex multi-site architectures. By leveraging in-memory computing, MongoDB provides high performance for both reads and writes. MongoDB’s native replication and automated failover enable enterprise-grade reliability and operational flexibility.

MongoDB is an open-source database used by companies of all sizes, across all industries and for a wide variety of applications. It is an agile database that allows schemas to change quickly as applications evolve, while still providing the functionality developers expect from traditional databases, such as secondary indexes, a full query language and strict consistency.

MongoDB has a rich client ecosystem including hadoop integration, officially supported drivers for 10 programming languages and environments, as well as 40 drivers supported by the user community.

MongoDB features:
* JSON Data Model with Dynamic Schemas
* Auto-Sharding for Horizontal Scalability
* Built-In Replication for High Availability
* Rich Secondary Indexes, including geospatial
* TTL indexes
* Text Search
* Aggregation Framework & Native MapReduce

This metapackage will install the mongo shell, import/export tools, other client utilities, server software, default configuration, and systemd service files.

%package -n mongodb-enterprise-unstable
Summary: MongoDB open source document-oriented database system (enterprise metapackage)
Group: Applications/Databases
Requires: mongodb-enterprise-unstable-database, mongodb-enterprise-unstable-tools, mongodb-mongosh
Conflicts: mongo-10gen, mongo-10gen-enterprise, mongo-10gen-enterprise-server, mongo-10gen-server, mongo-10gen-unstable, mongo-10gen-unstable-enterprise, mongo-10gen-unstable-enterprise-mongos, mongo-10gen-unstable-enterprise-server, mongo-10gen-unstable-enterprise-shell, mongo-10gen-unstable-enterprise-tools, mongo-10gen-unstable-mongos, mongo-10gen-unstable-server, mongo-10gen-unstable-shell, mongo-10gen-unstable-tools, mongo18-10gen, mongo18-10gen-server, mongo20-10gen, mongo20-10gen-server, mongodb, mongodb-server, mongodb-dev, mongodb-clients, mongodb-10gen, mongodb-10gen-enterprise, mongodb-10gen-unstable, mongodb-10gen-unstable-enterprise, mongodb-10gen-unstable-enterprise-mongos, mongodb-10gen-unstable-enterprise-server, mongodb-10gen-unstable-enterprise-shell, mongodb-10gen-unstable-enterprise-tools, mongodb-10gen-unstable-mongos, mongodb-10gen-unstable-server, mongodb-10gen-unstable-shell, mongodb-10gen-unstable-tools, mongodb-enterprise, mongodb-enterprise-mongos, mongodb-enterprise-server, mongodb-enterprise-shell, mongodb-enterprise-tools, mongodb-enterprise-cryptd, mongodb-nightly, mongodb-org, mongodb-org-mongos, mongodb-org-server, mongodb-org-shell, mongodb-org-tools, mongodb-stable, mongodb18-10gen, mongodb20-10gen, mongodb-org-unstable, mongodb-org-unstable-mongos, mongodb-org-unstable-server, mongodb-org-unstable-shell, mongodb-org-unstable-tools
Provides: mongo-10gen-enterprise-unstable

%description -n mongodb-enterprise-unstable
MongoDB is built for scalability, performance and high availability, scaling from single server deployments to large, complex multi-site architectures. By leveraging in-memory computing, MongoDB provides high performance for both reads and writes. MongoDB’s native replication and automated failover enable enterprise-grade reliability and operational flexibility.

MongoDB is an open-source database used by companies of all sizes, across all industries and for a wide variety of applications. It is an agile database that allows schemas to change quickly as applications evolve, while still providing the functionality developers expect from traditional databases, such as secondary indexes, a full query language and strict consistency.

MongoDB has a rich client ecosystem including hadoop integration, officially supported drivers for 10 programming languages and environments, as well as 40 drivers supported by the user community.

MongoDB features:
* JSON Data Model with Dynamic Schemas
* Auto-Sharding for Horizontal Scalability
* Built-In Replication for High Availability
* Rich Secondary Indexes, including geospatial
* TTL indexes
* Text Search
* Aggregation Framework & Native MapReduce

This metapackage will install the mongo shell, import/export tools, other client utilities, server software, default configuration, and systemd service files.

%package -n mongodb-enterprise-unstable-server
Summary: MongoDB database server (enterprise)
Group: Applications/Databases
Requires: openssl, net-snmp, cyrus-sasl, cyrus-sasl-plain, cyrus-sasl-gssapi, %{timezone_pkg}
Conflicts: mongo-10gen, mongo-10gen-enterprise, mongo-10gen-enterprise-server, mongo-10gen-server, mongo-10gen-unstable, mongo-10gen-unstable-enterprise, mongo-10gen-unstable-enterprise-mongos, mongo-10gen-unstable-enterprise-server, mongo-10gen-unstable-enterprise-shell, mongo-10gen-unstable-enterprise-tools, mongo-10gen-unstable-mongos, mongo-10gen-unstable-server, mongo-10gen-unstable-shell, mongo-10gen-unstable-tools, mongo18-10gen, mongo18-10gen-server, mongo20-10gen, mongo20-10gen-server, mongodb, mongodb-server, mongodb-dev, mongodb-clients, mongodb-10gen, mongodb-10gen-enterprise, mongodb-10gen-unstable, mongodb-10gen-unstable-enterprise, mongodb-10gen-unstable-enterprise-mongos, mongodb-10gen-unstable-enterprise-server, mongodb-10gen-unstable-enterprise-shell, mongodb-10gen-unstable-enterprise-tools, mongodb-10gen-unstable-mongos, mongodb-10gen-unstable-server, mongodb-10gen-unstable-shell, mongodb-10gen-unstable-tools, mongodb-enterprise, mongodb-enterprise-mongos, mongodb-enterprise-server, mongodb-enterprise-shell, mongodb-enterprise-tools, mongodb-nightly, mongodb-org, mongodb-org-mongos, mongodb-org-server, mongodb-org-shell, mongodb-org-tools, mongodb-stable, mongodb18-10gen, mongodb20-10gen, mongodb-org-unstable, mongodb-org-unstable-mongos, mongodb-org-unstable-server, mongodb-org-unstable-shell, mongodb-org-unstable-tools

%if 0%{?suse_version} >= 1210 || 0%{?rhel} >= 700 || 0%{?fedora} >= 15
BuildRequires: systemd-rpm-macros
%else
BuildRequires: systemd
%{?systemd_requires}
%endif

%description -n mongodb-enterprise-unstable-server
MongoDB is built for scalability, performance and high availability, scaling from single server deployments to large, complex multi-site architectures. By leveraging in-memory computing, MongoDB provides high performance for both reads and writes. MongoDB’s native replication and automated failover enable enterprise-grade reliability and operational flexibility.

MongoDB is an open-source database used by companies of all sizes, across all industries and for a wide variety of applications. It is an agile database that allows schemas to change quickly as applications evolve, while still providing the functionality developers expect from traditional databases, such as secondary indexes, a full query language and strict consistency.

MongoDB has a rich client ecosystem including hadoop integration, officially supported drivers for 10 programming languages and environments, as well as 40 drivers supported by the user community.

MongoDB features:
* JSON Data Model with Dynamic Schemas
* Auto-Sharding for Horizontal Scalability
* Built-In Replication for High Availability
* Rich Secondary Indexes, including geospatial
* TTL indexes
* Text Search
* Aggregation Framework & Native MapReduce

This package contains the MongoDB server software, default configuration files, and systemd service files.

%package -n mongodb-enterprise-unstable-mongos
Summary: MongoDB sharded cluster query router (enterprise)
Group: Applications/Databases
Requires: %{timezone_pkg}
Conflicts: mongo-10gen, mongo-10gen-enterprise, mongo-10gen-enterprise-server, mongo-10gen-server, mongo-10gen-unstable, mongo-10gen-unstable-enterprise, mongo-10gen-unstable-enterprise-mongos, mongo-10gen-unstable-enterprise-server, mongo-10gen-unstable-enterprise-shell, mongo-10gen-unstable-enterprise-tools, mongo-10gen-unstable-mongos, mongo-10gen-unstable-server, mongo-10gen-unstable-shell, mongo-10gen-unstable-tools, mongo18-10gen, mongo18-10gen-server, mongo20-10gen, mongo20-10gen-server, mongodb, mongodb-server, mongodb-dev, mongodb-clients, mongodb-10gen, mongodb-10gen-enterprise, mongodb-10gen-unstable, mongodb-10gen-unstable-enterprise, mongodb-10gen-unstable-enterprise-mongos, mongodb-10gen-unstable-enterprise-server, mongodb-10gen-unstable-enterprise-shell, mongodb-10gen-unstable-enterprise-tools, mongodb-10gen-unstable-mongos, mongodb-10gen-unstable-server, mongodb-10gen-unstable-shell, mongodb-10gen-unstable-tools, mongodb-enterprise, mongodb-enterprise-mongos, mongodb-enterprise-server, mongodb-enterprise-shell, mongodb-enterprise-tools, mongodb-nightly, mongodb-org, mongodb-org-mongos, mongodb-org-server, mongodb-org-shell, mongodb-org-tools, mongodb-stable, mongodb18-10gen, mongodb20-10gen, mongodb-org-unstable, mongodb-org-unstable-mongos, mongodb-org-unstable-server, mongodb-org-unstable-shell, mongodb-org-unstable-tools

%description -n mongodb-enterprise-unstable-mongos
MongoDB is built for scalability, performance and high availability, scaling from single server deployments to large, complex multi-site architectures. By leveraging in-memory computing, MongoDB provides high performance for both reads and writes. MongoDB’s native replication and automated failover enable enterprise-grade reliability and operational flexibility.

MongoDB is an open-source database used by companies of all sizes, across all industries and for a wide variety of applications. It is an agile database that allows schemas to change quickly as applications evolve, while still providing the functionality developers expect from traditional databases, such as secondary indexes, a full query language and strict consistency.

MongoDB has a rich client ecosystem including hadoop integration, officially supported drivers for 10 programming languages and environments, as well as 40 drivers supported by the user community.

MongoDB features:
* JSON Data Model with Dynamic Schemas
* Auto-Sharding for Horizontal Scalability
* Built-In Replication for High Availability
* Rich Secondary Indexes, including geospatial
* TTL indexes
* Text Search
* Aggregation Framework & Native MapReduce

This package contains mongos, the MongoDB sharded cluster query router.

%package -n mongodb-enterprise-unstable-tools
Summary: MongoDB tools metapackage (enterprise)
Group: Applications/Databases
Requires: mongodb-database-tools, mongodb-enterprise-unstable-database-tools-extra
Conflicts: mongo-10gen, mongo-10gen-enterprise, mongo-10gen-enterprise-server, mongo-10gen-server, mongo-10gen-unstable, mongo-10gen-unstable-enterprise, mongo-10gen-unstable-enterprise-mongos, mongo-10gen-unstable-enterprise-server, mongo-10gen-unstable-enterprise-shell, mongo-10gen-unstable-enterprise-tools, mongo-10gen-unstable-mongos, mongo-10gen-unstable-server, mongo-10gen-unstable-shell, mongo-10gen-unstable-tools, mongo18-10gen, mongo18-10gen-server, mongo20-10gen, mongo20-10gen-server, mongodb, mongodb-server, mongodb-dev, mongodb-clients, mongodb-10gen, mongodb-10gen-enterprise, mongodb-10gen-unstable, mongodb-10gen-unstable-enterprise, mongodb-10gen-unstable-enterprise-mongos, mongodb-10gen-unstable-enterprise-server, mongodb-10gen-unstable-enterprise-shell, mongodb-10gen-unstable-enterprise-tools, mongodb-10gen-unstable-mongos, mongodb-10gen-unstable-server, mongodb-10gen-unstable-shell, mongodb-10gen-unstable-tools, mongodb-enterprise, mongodb-enterprise-mongos, mongodb-enterprise-server, mongodb-enterprise-shell, mongodb-enterprise-tools, mongodb-nightly, mongodb-org, mongodb-org-mongos, mongodb-org-server, mongodb-org-shell, mongodb-org-tools, mongodb-stable, mongodb18-10gen, mongodb20-10gen, mongodb-org-unstable, mongodb-org-unstable-mongos, mongodb-org-unstable-server, mongodb-org-unstable-shell, mongodb-org-unstable-tools

%description -n mongodb-enterprise-unstable-tools
MongoDB is built for scalability, performance and high availability, scaling from single server deployments to large, complex multi-site architectures. By leveraging in-memory computing, MongoDB provides high performance for both reads and writes. MongoDB’s native replication and automated failover enable enterprise-grade reliability and operational flexibility.

MongoDB is an open-source database used by companies of all sizes, across all industries and for a wide variety of applications. It is an agile database that allows schemas to change quickly as applications evolve, while still providing the functionality developers expect from traditional databases, such as secondary indexes, a full query language and strict consistency.

MongoDB has a rich client ecosystem including hadoop integration, officially supported drivers for 10 programming languages and environments, as well as 40 drivers supported by the user community.

MongoDB features:
* JSON Data Model with Dynamic Schemas
* Auto-Sharding for Horizontal Scalability
* Built-In Replication for High Availability
* Rich Secondary Indexes, including geospatial
* TTL indexes
* Text Search
* Aggregation Framework & Native MapReduce

This metapackage exists to simplfy acquisition of both the database tools and the extra database tools.

%package -n mongodb-enterprise-unstable-database-tools-extra
Summary: MongoDB extra database tools (enterprise)
Group: Applications/Databases
Requires: openssl, cyrus-sasl, cyrus-sasl-plain, cyrus-sasl-gssapi, %{python_pkg}
Conflicts: mongo-10gen, mongo-10gen-enterprise, mongo-10gen-enterprise-server, mongo-10gen-server, mongo-10gen-unstable, mongo-10gen-unstable-enterprise, mongo-10gen-unstable-enterprise-mongos, mongo-10gen-unstable-enterprise-server, mongo-10gen-unstable-enterprise-shell, mongo-10gen-unstable-enterprise-tools, mongo-10gen-unstable-mongos, mongo-10gen-unstable-server, mongo-10gen-unstable-shell, mongo-10gen-unstable-tools, mongo18-10gen, mongo18-10gen-server, mongo20-10gen, mongo20-10gen-server, mongodb, mongodb-server, mongodb-dev, mongodb-clients, mongodb-10gen, mongodb-10gen-enterprise, mongodb-10gen-unstable, mongodb-10gen-unstable-enterprise, mongodb-10gen-unstable-enterprise-mongos, mongodb-10gen-unstable-enterprise-server, mongodb-10gen-unstable-enterprise-shell, mongodb-10gen-unstable-enterprise-tools, mongodb-10gen-unstable-mongos, mongodb-10gen-unstable-server, mongodb-10gen-unstable-shell, mongodb-10gen-unstable-tools, mongodb-enterprise, mongodb-enterprise-mongos, mongodb-enterprise-server, mongodb-enterprise-shell, mongodb-enterprise-tools, mongodb-nightly, mongodb-org, mongodb-org-mongos, mongodb-org-server, mongodb-org-shell, mongodb-org-tools, mongodb-stable, mongodb18-10gen, mongodb20-10gen, mongodb-org-unstable, mongodb-org-unstable-mongos, mongodb-org-unstable-server, mongodb-org-unstable-shell, mongodb-org-unstable-tools, mongodb-org-database-tools-extra, mongodb-org-unstable-database-tools-extra, mongodb-enterprise-database-tools-extra, mongodb-enterprise-unstable-tools <= 4.2

%description -n mongodb-enterprise-unstable-database-tools-extra
MongoDB is built for scalability, performance and high availability, scaling from single server deployments to large, complex multi-site architectures. By leveraging in-memory computing, MongoDB provides high performance for both reads and writes. MongoDB’s native replication and automated failover enable enterprise-grade reliability and operational flexibility.

MongoDB is an open-source database used by companies of all sizes, across all industries and for a wide variety of applications. It is an agile database that allows schemas to change quickly as applications evolve, while still providing the functionality developers expect from traditional databases, such as secondary indexes, a full query language and strict consistency.

MongoDB has a rich client ecosystem including hadoop integration, officially supported drivers for 10 programming languages and environments, as well as 40 drivers supported by the user community.

MongoDB features:
* JSON Data Model with Dynamic Schemas
* Auto-Sharding for Horizontal Scalability
* Built-In Replication for High Availability
* Rich Secondary Indexes, including geospatial
* TTL indexes
* Text Search
* Aggregation Framework & Native MapReduce

This package contains extra database tools, like the Compass installer, mongodecrypt, mongoldap, and mongokerberos.

%package -n mongodb-enterprise-unstable-cryptd
Summary: MongoDB Client Side Field Level Encryption Support Daemon (enterprise)
Group: Applications/Databases
Requires: openssl, cyrus-sasl
Conflicts: mongodb-enterprise-cryptd

%description -n mongodb-enterprise-unstable-cryptd
MongoDB is built for scalability, performance and high availability, scaling from single server deployments to large, complex multi-site architectures. By leveraging in-memory computing, MongoDB provides high performance for both reads and writes. MongoDB’s native replication and automated failover enable enterprise-grade reliability and operational flexibility.

MongoDB is an open-source database used by companies of all sizes, across all industries and for a wide variety of applications. It is an agile database that allows schemas to change quickly as applications evolve, while still providing the functionality developers expect from traditional databases, such as secondary indexes, a full query language and strict consistency.

MongoDB has a rich client ecosystem including hadoop integration, officially supported drivers for 10 programming languages and environments, as well as 40 drivers supported by the user community.

MongoDB features:
* JSON Data Model with Dynamic Schemas
* Auto-Sharding for Horizontal Scalability
* Built-In Replication for High Availability
* Rich Secondary Indexes, including geospatial
* TTL indexes
* Text Search
* Aggregation Framework & Native MapReduce

This package contains mongocryptd, the Client Side Field Level Encryption Support Daemon.

%package -n mongodb-enterprise-unstable-devel
Summary: Headers and libraries for MongoDB development.
Group: Applications/Databases
Conflicts: mongo-10gen, mongo-10gen-enterprise, mongo-10gen-enterprise-server, mongo-10gen-server, mongo-10gen-unstable, mongo-10gen-unstable-enterprise, mongo-10gen-unstable-enterprise-mongos, mongo-10gen-unstable-enterprise-server, mongo-10gen-unstable-enterprise-shell, mongo-10gen-unstable-enterprise-tools, mongo-10gen-unstable-mongos, mongo-10gen-unstable-server, mongo-10gen-unstable-shell, mongo-10gen-unstable-tools, mongo18-10gen, mongo18-10gen-server, mongo20-10gen, mongo20-10gen-server, mongodb, mongodb-server, mongodb-dev, mongodb-clients, mongodb-10gen, mongodb-10gen-enterprise, mongodb-10gen-unstable, mongodb-10gen-unstable-enterprise, mongodb-10gen-unstable-enterprise-mongos, mongodb-10gen-unstable-enterprise-server, mongodb-10gen-unstable-enterprise-shell, mongodb-10gen-unstable-enterprise-tools, mongodb-10gen-unstable-mongos, mongodb-10gen-unstable-server, mongodb-10gen-unstable-shell, mongodb-10gen-unstable-tools, mongodb-enterprise, mongodb-enterprise-mongos, mongodb-enterprise-server, mongodb-enterprise-shell, mongodb-enterprise-tools, mongodb-nightly, mongodb-org, mongodb-org-mongos, mongodb-org-server, mongodb-org-shell, mongodb-org-tools, mongodb-stable, mongodb18-10gen, mongodb20-10gen, mongodb-org-unstable, mongodb-org-unstable-mongos, mongodb-org-unstable-server, mongodb-org-unstable-shell, mongodb-org-unstable-tools

%description -n mongodb-enterprise-unstable-devel
MongoDB is built for scalability, performance and high availability, scaling from single server deployments to large, complex multi-site architectures. By leveraging in-memory computing, MongoDB provides high performance for both reads and writes. MongoDB’s native replication and automated failover enable enterprise-grade reliability and operational flexibility.

MongoDB is an open-source database used by companies of all sizes, across all industries and for a wide variety of applications. It is an agile database that allows schemas to change quickly as applications evolve, while still providing the functionality developers expect from traditional databases, such as secondary indexes, a full query language and strict consistency.

MongoDB has a rich client ecosystem including hadoop integration, officially supported drivers for 10 programming languages and environments, as well as 40 drivers supported by the user community.

MongoDB features:
* JSON Data Model with Dynamic Schemas
* Auto-Sharding for Horizontal Scalability
* Built-In Replication for High Availability
* Rich Secondary Indexes, including geospatial
* TTL indexes
* Text Search
* Aggregation Framework & Native MapReduce

This package provides the MongoDB static library and header files needed to develop MongoDB client software.

#Release builds have no debug symbols, and this prevents packaging errors on RHEL 8.0
%global debug_package %{nil}

%prep
%setup -n %{_name}-%{version}
%if 0%{?rhel} >= 8 || 0%{?fedora} >= 30
pathfix.py -pni "%{__python3} %{py3_shbang_opts}" bin/install_compass
%endif

%build

%install
mkdir -p $RPM_BUILD_ROOT%{_prefix}
cp -rv bin $RPM_BUILD_ROOT%{_prefix}
mkdir -p $RPM_BUILD_ROOT%{_mandir}/man1
cp debian/mongo{d,s,ldap,kerberos}.1 $RPM_BUILD_ROOT%{_mandir}/man1/
mkdir -p $RPM_BUILD_ROOT%{_mandir}/man5
cp debian/mongodb-parameters.5 $RPM_BUILD_ROOT%{_mandir}/man5/
mkdir -p $RPM_BUILD_ROOT%{_sysconfdir}
cp -v rpm/mongod.conf $RPM_BUILD_ROOT%{_sysconfdir}/mongod.conf
mkdir -p $RPM_BUILD_ROOT%{_unitdir}
cp -v rpm/mongod.service $RPM_BUILD_ROOT%{_unitdir}
mkdir -p $RPM_BUILD_ROOT%{_sharedstatedir}/mongo
mkdir -p $RPM_BUILD_ROOT%{_localstatedir}/log/mongodb
mkdir -p $RPM_BUILD_ROOT%{_rundir}/mongodb
touch $RPM_BUILD_ROOT%{_localstatedir}/log/mongodb/mongod.log



%clean
rm -rf $RPM_BUILD_ROOT

%pre -n mongodb-enterprise-unstable-server
if ! /usr/bin/id -g mongod &>/dev/null; then
    /usr/sbin/groupadd -r mongod
fi
if ! /usr/bin/id mongod &>/dev/null; then
    /usr/sbin/useradd -M -r -g mongod -d /var/lib/mongo -s /bin/false   -c mongod mongod > /dev/null 2>&1
fi

%post -n mongodb-enterprise-unstable-server
if test $1 = 1
then
  /usr/bin/systemctl enable mongod
fi
if test $1 = 2
then
  /usr/bin/systemctl daemon-reload
fi


%preun -n mongodb-enterprise-unstable-server
if test $1 = 0
then
  /usr/bin/systemctl disable mongod
fi

%postun -n mongodb-enterprise-unstable-server
if test $1 -ge 1
then
  /usr/bin/systemctl restart mongod >/dev/null 2>&1 || :
fi

%files

%files -n mongodb-enterprise-unstable

%files -n mongodb-enterprise-unstable-server
%defattr(-,root,root,-)
%config(noreplace) %{_sysconfdir}/mongod.conf
%{_bindir}/mongod
%{_mandir}/man1/mongod.1*
%{_mandir}/man5/mongodb-parameters.5*
%{_unitdir}/mongod.service
%attr(0755,mongod,mongod) %dir %{_sharedstatedir}/mongo
%attr(0755,mongod,mongod) %dir %{_localstatedir}/log/mongodb
%attr(0755,mongod,mongod) %dir %{_rundir}/mongodb
%attr(0640,mongod,mongod) %config(noreplace) %verify(not md5 size mtime) %{_localstatedir}/log/mongodb/mongod.log
%doc LICENSE-Enterprise.txt
%doc README
%doc THIRD-PARTY-NOTICES
%doc MPL-2

%files -n mongodb-enterprise-unstable-mongos
%defattr(-,root,root,-)
%{_bindir}/mongos
%{_mandir}/man1/mongos.1*

%files -n mongodb-enterprise-unstable-tools

%files -n mongodb-enterprise-unstable-database-tools-extra
%defattr(-,root,root,-)

%{_bindir}/install_compass
%{_bindir}/mongodecrypt
%{_bindir}/mongoldap
%{_bindir}/mongokerberos

%{_mandir}/man1/mongoldap.1*
%{_mandir}/man1/mongokerberos.1*

%files -n mongodb-enterprise-unstable-cryptd
%defattr(-,root,root,-)
%{_bindir}/mongocryptd

%changelog
* Wed May 13 2020 MongoDB Packaging <packaging@mongodb.com>
- Update packaging contact

* Mon Oct 10 2016 Sam Kleinman <sam@mongodb.com>
- Support for systemd init processes.

* Thu Dec 19 2013 Ernie Hershey <ernie.hershey@mongodb.com>
- Packaging file cleanup

* Thu Jan 28 2010 Richard M Kreuter <richard@10gen.com>
- Minor fixes.

* Sat Oct 24 2009 Joe Miklojcik <jmiklojcik@shopwiki.com> -
- Wrote mongo.spec.
