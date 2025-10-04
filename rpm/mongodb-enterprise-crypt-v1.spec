#Release builds have no debug symbols, and this prevents packaging errors on RHEL 8
%global   debug_package %{nil}
%define   _name mongodb-enterprise
%define   _crypto_pkg_name %{_name}-crypt-v1

Name:      %{_crypto_pkg_name}
Version:   %{dynamic_version}
Release:   %{dynamic_release}%{?dist}
Summary:   MongoDB Crypto v1 Library
License:   Commercial
Group:     Applications/Databases
Conflicts: %{_name}-unstable-crypt-v1
Source:    %{_crypto_pkg_name}-%{version}.tar.gz
BuildRoot: %{_tmppath}/%{_crypto_pkg_name}-%{version}-%{release}-root

%description
MongoDB is built for scalability, performance and high availability, scaling from single server deployments to large, complex multi-site architectures. By leveraging in-memory computing, MongoDB provides high performance for both reads and writes. MongoDB▒~@~Ys native replication and automated failover enable enterprise-grade reliability and operational flexibility.

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

This package contains MongoDB Enterprise Field Level Encryption Crypto v1 library.

%prep

%setup

%install
mkdir -p $RPM_BUILD_ROOT%{_includedir}/mongo_crypt/v1/mongo_crypt
cp include/mongo_crypt/v1/mongo_crypt/mongo_crypt.h $RPM_BUILD_ROOT%{_includedir}/mongo_crypt/v1/mongo_crypt/

mkdir -p $RPM_BUILD_ROOT%{_libdir}
cp lib/mongo_crypt_v1.so $RPM_BUILD_ROOT%{_libdir}/

%clean
rm -rf $RPM_BUILD_ROOT

%files
%defattr(-,root,root,-)
%attr(0755,root,root) %dir %{_includedir}/mongo_crypt
%attr(0755,root,root) %dir %{_includedir}/mongo_crypt/v1
%attr(0755,root,root) %dir %{_includedir}/mongo_crypt/v1/mongo_crypt
%attr(0644,root,root)      %{_includedir}/mongo_crypt/v1/mongo_crypt/mongo_crypt.h
%attr(0755,root,root)      %{_libdir}/mongo_crypt_v1.so
%doc LICENSE-Enterprise.txt
%doc README
%doc THIRD-PARTY-NOTICES
%doc MPL-2
