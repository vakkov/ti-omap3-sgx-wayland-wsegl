# 
# Do NOT Edit the Auto-generated Part!
# Generated by: spectacle version 0.26
# 

Name:       ti-omap3-sgx-wayland-wsegl

# >> macros
# << macros

Summary:    WSEGL for Wayland
Version:    0.1
Release:    1
Group:      Applications/Engineering
License:    Undecided
URL:        http://foobar.com
Source0:    %{name}-%{version}.tar.gz
Source100:  ti-omap3-sgx-wayland-wsegl.yaml
Requires:   ti-omap3-sgx >= 1.4.268.2
Requires(post): /sbin/ldconfig
Requires(postun): /sbin/ldconfig
BuildRequires:  pkgconfig(libffi)
BuildRequires:  pkgconfig(wayland-client)
BuildRequires:  pkgconfig(wayland-server)
BuildRequires:  ti-omap3-sgx-devel
BuildRequires:  ti-omap3-sgx-libEGL-devel
# BuildRequires:  qt-devel
Provides:   libEGL
Provides:   libEGL.so.1
Conflicts:   ti-omap3-sgx <= 1.4.268.1
Conflicts:   mesa-llvmpipe-libEGL

%description
- Something


%prep
%setup -q -n %{name}-%{version}

# >> setup
# << setup

%build
# >> build pre
# << build pre

%qmake5

make %{?jobs:-j%jobs}

# >> build post
# << build post

%install
rm -rf %{buildroot}
# >> install pre
# << install pre
%qmake_install

# >> install post
# << install post

%post -p /sbin/ldconfig

%postun -p /sbin/ldconfig

%files
%defattr(-,root,root,-)
%{_libdir}/waylandwsegl.so
%{_libdir}/libEGL.so
%{_libdir}/libEGL.so.1
%{_libdir}/libEGL.so.1.0
%{_libdir}/libEGL.so.1.0.0
%{_libdir}/libwayland-egl.so
%{_libdir}/libwayland-egl.so.1
%{_libdir}/libwayland-egl.so.1.0
%{_libdir}/libwayland-egl.so.1.0.0
# >> files
# << files
