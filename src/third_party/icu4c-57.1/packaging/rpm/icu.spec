#   Copyright (C) 2000-2005, International Business Machines
#   Corporation and others.  All Rights Reserved.
#
# RPM specification file for ICU.
#
# Neal Probert <nprobert@walid.com> is the current maintainer.
# Yves Arrouye <yves@realnames.com> is the original author.

# This file can be freely redistributed under the same license as ICU.

Name: icu
Version: 3.4
Release: 1
Requires: libicu34 >= %{version}
Summary: International Components for Unicode
Packager: Ian Holsman (CNET Networks) <ianh@cnet.com>
Copyright: X License
Group: System Environment/Libraries
Source: icu-%{version}.tgz
BuildRoot: /var/tmp/%{name}-%{version}
%description
ICU is a set of C and C++ libraries that provides robust and full-featured
Unicode and locale support. The library provides calendar support, conversions
for many character sets, language sensitive collation, date
and time formatting, support for many locales, message catalogs
and resources, message formatting, normalization, number and currency
formatting, time zones support, transliteration, word, line and
sentence breaking, etc.

This package contains the Unicode character database and derived
properties, along with converters and time zones data.

This package contains the runtime libraries for ICU. It does
not contain any of the data files needed at runtime and present in the
`icu' and `icu-locales` packages.

%package -n libicu34
Summary: International Components for Unicode (libraries)
Group: Development/Libraries
%description -n libicu34
ICU is a set of C and C++ libraries that provides robust and full-featured
Unicode support. This package contains the runtime libraries for ICU. It does
not contain any of the data files needed at runtime and present in the
`icu' and `icu-locales` packages.

%package -n libicu-devel
Summary: International Components for Unicode (development files)
Group: Development/Libraries
Requires: libicu34 = %{version}
%description -n libicu-devel
ICU is a set of C and C++ libraries that provides robust and full-featured
Unicode support. This package contains the development files for ICU.

%package locales
Summary: Locale data for ICU
Group: System Environment/Libraries
Requires: libicu34 >= %{version}
%description locales
The locale data are used by ICU to provide localization (l10n), 
internationalization (i18n) and timezone support to ICU applications.
This package also contains break data for various languages,
and transliteration data.

%post
# Adjust the current ICU link in /usr/lib/icu

icucurrent=`2>/dev/null ls -dp /usr/lib/icu/* | sed -n 's,.*/\([^/]*\)/$,\1,p'| sort -rn | head -1`
cd /usr/lib/icu
rm -f /usr/lib/icu/current
if test x"$icucurrent" != x
then
    ln -s "$icucurrent" current
fi

#ICU_DATA=/usr/share/icu/%{version}
#export ICU_DATA

%preun
# Adjust the current ICU link in /usr/lib/icu

icucurrent=`2>/dev/null ls -dp /usr/lib/icu/* | sed -n -e '/\/%{version}\//d' -e 's,.*/\([^/]*\)/$,\1,p'| sort -rn | head -1`
cd /usr/lib/icu
rm -f /usr/lib/icu/current
if test x"$icucurrent" != x
then
    ln -s "$icucurrent" current
fi

%post -n libicu34
ldconfig

# Adjust the current ICU link in /usr/lib/icu

icucurrent=`2>/dev/null ls -dp /usr/lib/icu/* | sed -n 's,.*/\([^/]*\)/$,\1,p'| sort -rn | head -1`
cd /usr/lib/icu
rm -f /usr/lib/icu/current
if test x"$icucurrent" != x
then
    ln -s "$icucurrent" current
fi

%preun -n libicu34
# Adjust the current ICU link in /usr/lib/icu

icucurrent=`2>/dev/null ls -dp /usr/lib/icu/* | sed -n -e '/\/%{version}\//d' -e 's,.*/\([^/]*\)/$,\1,p'| sort -rn | head -1`
cd /usr/lib/icu
rm -f /usr/lib/icu/current
if test x"$icucurrent" != x
then
    ln -s "$icucurrent" current
fi

%prep
%setup -q -n icu

%build
cd source
chmod a+x ./configure
CFLAGS="-O3" CXXFLAGS="-O" ./configure --prefix=/usr --sysconfdir=/etc --with-data-packaging=files --enable-shared --enable-static --disable-samples
echo 'CPPFLAGS += -DICU_DATA_DIR=\"/usr/share/icu/%{version}\"' >> icudefs.mk
make RPM_OPT_FLAGS="$RPM_OPT_FLAGS"

%install
rm -rf $RPM_BUILD_ROOT
cd source
make install DESTDIR=$RPM_BUILD_ROOT

%files
%defattr(-,root,root)
%doc readme.html
%doc license.html
/usr/share/icu/%{version}/license.html
/usr/share/icu/%{version}/icudt34l/*.cnv
/usr/share/icu/%{version}/icudt34l/*.icu
/usr/share/icu/%{version}/icudt34l/*.spp

/usr/bin/derb
/usr/bin/genbrk
/usr/bin/gencnval
/usr/bin/genrb
/usr/bin/icu-config
/usr/bin/makeconv
/usr/bin/pkgdata
/usr/bin/uconv

/usr/sbin/decmn
/usr/sbin/genccode
/usr/sbin/gencmn
/usr/sbin/gensprep
/usr/sbin/genuca
/usr/sbin/icuswap
/usr/share/icu/%{version}/mkinstalldirs

/usr/man/man1/derb.1.*
/usr/man/man1/gencnval.1.*
/usr/man/man1/genrb.1.*
/usr/man/man1/icu-config.1.*
/usr/man/man1/makeconv.1.*
/usr/man/man1/pkgdata.1.*
/usr/man/man1/uconv.1.*
/usr/man/man8/decmn.8.*
/usr/man/man8/genccode.8.*
/usr/man/man8/gencmn.8.*
/usr/man/man8/gensprep.8.*
/usr/man/man8/genuca.8.*

%files -n icu-locales
/usr/share/icu/%{version}/icudt34l/*.brk
/usr/share/icu/%{version}/icudt34l/*.res
/usr/share/icu/%{version}/icudt34l/coll/*.res
/usr/share/icu/%{version}/icudt34l/rbnf/*.res
/usr/share/icu/%{version}/icudt34l/translit/*.res

%files -n libicu34
%doc license.html
/usr/lib/libicui18n.so.34
/usr/lib/libicui18n.so.34.0
/usr/lib/libicutu.so.34
/usr/lib/libicutu.so.34.0
/usr/lib/libicuuc.so.34
/usr/lib/libicuuc.so.34.0
/usr/lib/libicudata.so.34
/usr/lib/libicudata.so.34.0
/usr/lib/libicuio.so.34
/usr/lib/libicuio.so.34.0
/usr/lib/libiculx.so.34
/usr/lib/libiculx.so.34.0
/usr/lib/libicule.so.34
/usr/lib/libicule.so.34.0

%files -n libicu-devel
%doc readme.html
%doc license.html
/usr/lib/libicui18n.so
/usr/lib/libsicui18n.a
/usr/lib/libicuuc.so
/usr/lib/libsicuuc.a
/usr/lib/libicutu.so
/usr/lib/libsicutu.a
/usr/lib/libicuio.so
/usr/lib/libsicuio.a
/usr/lib/libicudata.so
/usr/lib/libsicudata.a
/usr/lib/libicule.so
/usr/lib/libsicule.a
/usr/lib/libiculx.so
/usr/lib/libsiculx.a
/usr/include/unicode/*.h
/usr/include/layout/*.h
/usr/lib/icu/%{version}/Makefile.inc
/usr/lib/icu/Makefile.inc
/usr/share/icu/%{version}/config
/usr/share/doc/icu-%{version}/*

%changelog
* Mon Jun 07 2004 Alexei Dets <adets@idsk.com>
- update to 3.0
* Tue Aug 16 2003 Steven Loomis <srl@jtcsv.com>
- update to 2.6.1 - include license
* Thu Jun 05 2003 Steven Loomis <srl@jtcsv.com>
- Update to 2.6
* Fri Dec 27 2002 Steven Loomis <srl@jtcsv.com>
- Update to 2.4 spec
* Fri Sep 27 2002 Steven Loomis <srl@jtcsv.com>
- minor updates to 2.2 spec. Rpath is off by default, don't pass it as an option.
* Mon Sep 16 2002 Ian Holsman <ian@holsman.net> 
- update to icu 2.2

