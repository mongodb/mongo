Name:		wiredtiger
Version:	2.8.1
Release:	1%{?dist}
Summary:	WiredTiger data storage engine

Group:		Development/Libraries
License:	GPLV2 or GPLV3
URL:		www.wiredtiger.com
Source0:	http://source.wiredtiger.com/releases/%{name}-%{version}.tar.bz2
BuildRoot:	%(mktemp -ud %{_tmppath}/%{name}-%{version}-%{release}-XXXXXX)

BuildRequires:	python-devel java-devel
Requires:	jemalloc

%description

WiredTiger is a data storage engine that provides APIs for efficiently
storing data in highly concurrent applications. It includes functionality
for automatically maintaining indexes. It implements both row and column
store formats - so that all types of data can be stored space efficiently.

WiredTiger is a library that can be accessed via C, Python and Java APIs.


%prep
%autosetup


%build
%configure --enable-java --enable-bzip2 --enable-snappy --enable-zlib
# Stop the build setting up an rpath
sed -i 's|^hardcode_libdir_flag_spec=.*|hardcode_libdir_flag_spec=""|g' libtool
sed -i 's|^runpath_var=LD_RUN_PATH|runpath_var=DIE_RPATH_DIE|g' libtool
make %{?_smp_mflags}


%install
rm -rf %{buildroot}
make install DESTDIR=%{buildroot}
# Need to resolve make install with --enable-python before we can
# install the python API.
# python setup.py install -O1 --skip-build --root $RPM_BUILD_ROOT

%clean
rm -rf %{buildroot}


%files
%defattr(-,root,root,-)
%doc README LICENSE NEWS
%{_bindir}/*
%{_datadir}/*
%{_includedir}/*
%{_libdir}/*


%changelog

