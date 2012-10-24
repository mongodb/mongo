%define	RELEASE	1
%define rel     %{?CUSTOM_RELEASE} %{!?CUSTOM_RELEASE:%RELEASE}
%define	prefix	/usr

Name: %NAME
Summary: A commandline flags library that allows for distributed flags
Version: %VERSION
Release: %rel
Group: Development/Libraries
URL: http://code.google.com/p/gflags
License: BSD
Vendor: Google Inc. and others
Packager: Google Inc. and others <google-gflags@googlegroups.com>
Source: http://%{NAME}.googlecode.com/files/%{NAME}-%{VERSION}.tar.gz
Distribution: Redhat 7 and above.
Buildroot: %{_tmppath}/%{name}-root
Prefix: %prefix

%description
The %name package contains a library that implements commandline flags
processing.  As such it's a replacement for getopt().  It has increased
flexibility, including built-in support for C++ types like string, and
the ability to define flags in the source file in which they're used.

%package devel
Summary: A commandline flags library that allows for distributed flags
Group: Development/Libraries
Requires: %{NAME} = %{VERSION}

%description devel
The %name-devel package contains static and debug libraries and header
files for developing applications that use the %name package.

%changelog
	* Thu Sep 10 2009 <opensource@google.com>
        - Change from '%configure' to something like it, but without -m32

	* Mon Apr 20 2009 <opensource@google.com>
	- Change build rule to use '%configure' rather than './configure'
	- Change install to use DESTDIR instead of prefix for make install.
	- Use wildcards for doc/ and lib/ directories
        - Use {_libdir}/{_includedir}/etc instead of {prefix}/lib, etc

	* Tue Dec 13 2006 <opensource@google.com>
	- First draft

%prep
%setup

%build
# I can't use '% configure', because it defines -m32 which breaks the
# build somehow on my system.  But I do take as much from % configure
# (in /usr/lib/rpm/macros) as I can.
./configure --prefix=%{_prefix} --exec-prefix=%{_exec_prefix} --bindir=%{_bindir} --sbindir=%{_sbindir} --sysconfdir=%{_sysconfdir} --datadir=%{_datadir} --includedir=%{_includedir} --libdir=%{_libdir} --libexecdir=%{_libexecdir} --localstatedir=%{_localstatedir} --sharedstatedir=%{_sharedstatedir} --mandir=%{_mandir} --infodir=%{_infodir}
make

%install
rm -rf $RPM_BUILD_ROOT
make DESTDIR=$RPM_BUILD_ROOT install

%clean
rm -rf $RPM_BUILD_ROOT

%files
%defattr(-,root,root)

%docdir %{prefix}/share/doc/%{NAME}-%{VERSION}
%{prefix}/share/doc/%{NAME}-%{VERSION}/*

%{_libdir}/*.so.*
%{_bindir}/gflags_completions.sh

%files devel
%defattr(-,root,root)

%{_includedir}/gflags
%{_includedir}/google
%{_libdir}/*.a
%{_libdir}/*.la
%{_libdir}/*.so
%{_libdir}/pkgconfig/*.pc
