Name:          zeromq
Version:       @PACKAGE_VERSION@
Release:       1%{?dist}
Summary:       The ZeroMQ messaging library
Group:         Applications/Internet
License:       LGPLv3+
URL:           http://www.zeromq.org/
Source:        http://download.zeromq.org/%{name}-%{version}.tar.gz
Prefix:        %{_prefix}
Buildroot:     %{_tmppath}/%{name}-%{version}-%{release}-root
BuildRequires: gcc, make, gcc-c++, libstdc++-devel
Requires:      libstdc++

%if %{?rhel}%{!?rhel:0} >= 6
BuildRequires: libuuid-devel
Requires:      libuuid
%elseif %{?rhel}%{!?rhel:0} >= 5
BuildRequires: e2fsprogs-devel
Requires:      e2fsprogs
%else
BuildRequires: uuid-devel
Requires:      uuid
%endif

# Build pgm only on supported archs
%ifarch pentium3 pentium4 athlon i386 i486 i586 i686 x86_64
BuildRequires: glib2-devel
Requires: glib2
%endif

%description
The 0MQ lightweight messaging kernel is a library which extends the
standard socket interfaces with features traditionally provided by
specialised messaging middleware products. 0MQ sockets provide an
abstraction of asynchronous message queues, multiple messaging
patterns, message filtering (subscriptions), seamless access to
multiple transport protocols and more.

This package contains the ZeroMQ shared library.

%package devel
Summary:  Development files and static library for the ZeroMQ library
Group:    Development/Libraries
Requires: %{name} = %{version}-%{release}, pkgconfig

%description devel
The 0MQ lightweight messaging kernel is a library which extends the
standard socket interfaces with features traditionally provided by
specialised messaging middleware products. 0MQ sockets provide an
abstraction of asynchronous message queues, multiple messaging
patterns, message filtering (subscriptions), seamless access to
multiple transport protocols and more.

This package contains ZeroMQ related development libraries and header files.

%prep
%setup -q

%build
%ifarch pentium3 pentium4 athlon i386 i486 i586 i686 x86_64
  %configure --with-pgm
%else
  %configure
%endif

%{__make} %{?_smp_mflags}

%install
[ "%{buildroot}" != "/" ] && %{__rm} -rf %{buildroot}

# Install the package to build area
%{__make} check
%makeinstall

%post
/sbin/ldconfig

%postun
/sbin/ldconfig

%clean
[ "%{buildroot}" != "/" ] && %{__rm} -rf %{buildroot}

%files
%defattr(-,root,root,-)

# docs in the main package
%doc AUTHORS ChangeLog COPYING COPYING.LESSER NEWS README

# libraries
%{_libdir}/libzmq.so.1
%{_libdir}/libzmq.so.1.0.0

%{_mandir}/man7/zmq.7.gz

%files devel
%defattr(-,root,root,-)
%{_includedir}/zmq.h
%{_includedir}/zmq.hpp
%{_includedir}/zmq_utils.h

%{_libdir}/libzmq.la
%{_libdir}/libzmq.a
%{_libdir}/pkgconfig/libzmq.pc
%{_libdir}/libzmq.so

%{_mandir}/man3/zmq_bind.3.gz
%{_mandir}/man3/zmq_close.3.gz
%{_mandir}/man3/zmq_connect.3.gz
%{_mandir}/man3/zmq_errno.3.gz
%{_mandir}/man3/zmq_device.3.gz
%{_mandir}/man3/zmq_getsockopt.3.gz
%{_mandir}/man3/zmq_init.3.gz
%{_mandir}/man3/zmq_msg_close.3.gz
%{_mandir}/man3/zmq_msg_copy.3.gz
%{_mandir}/man3/zmq_msg_data.3.gz
%{_mandir}/man3/zmq_msg_init.3.gz
%{_mandir}/man3/zmq_msg_init_data.3.gz
%{_mandir}/man3/zmq_msg_init_size.3.gz
%{_mandir}/man3/zmq_msg_move.3.gz
%{_mandir}/man3/zmq_msg_size.3.gz
%{_mandir}/man3/zmq_poll.3.gz
%{_mandir}/man3/zmq_recv.3.gz
%{_mandir}/man3/zmq_send.3.gz
%{_mandir}/man3/zmq_setsockopt.3.gz
%{_mandir}/man3/zmq_socket.3.gz
%{_mandir}/man3/zmq_strerror.3.gz
%{_mandir}/man3/zmq_term.3.gz
%{_mandir}/man3/zmq_version.3.gz
%{_mandir}/man7/zmq_cpp.7.gz
%{_mandir}/man7/zmq_epgm.7.gz
%{_mandir}/man7/zmq_inproc.7.gz
%{_mandir}/man7/zmq_ipc.7.gz
%{_mandir}/man7/zmq_pgm.7.gz
%{_mandir}/man7/zmq_tcp.7.gz

%changelog
* Sat Apr 10 2010 Mikko Koppanen <mkoppanen@php.net> 2.0.7-1
- Initial packaging
