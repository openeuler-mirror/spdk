# Build documentation package
%bcond_with doc

Name: spdk
Version: 20.01.1
Release: 2
Summary: Set of libraries and utilities for high performance user-mode storage
License: BSD and MIT
URL: http://spdk.io
Source0: https://github.com/spdk/spdk/archive/v%{version}.tar.gz

%define package_version %{version}-%{release}

%define install_datadir %{buildroot}/%{_datadir}/%{name}
%define install_sbindir %{buildroot}/%{_sbindir}
%define install_docdir %{buildroot}/%{_docdir}/%{name}

# Distros that don't support python3 will use python2
%if "%{dist}" == ".el7"
%define use_python2 1
%else
%define use_python2 0
%endif

ExclusiveArch: x86_64 aarch64

BuildRequires: gcc gcc-c++ make
BuildRequires: dpdk-devel, numactl-devel
BuildRequires: libiscsi-devel, libaio-devel, openssl-devel, libuuid-devel
BuildRequires: libibverbs-devel, librdmacm-devel
%if %{with doc}
BuildRequires: doxygen mscgen graphviz
%endif

%ifarch aarch64
%global config arm64-armv8a-linux-gcc
%else
%global config x86_64-native-linux-gcc
%endif

# Install dependencies
Requires: dpdk >= 17.11, numactl-libs, openssl-libs
Requires: libiscsi, libaio, libuuid
# NVMe over Fabrics
Requires: librdmacm, librdmacm
Requires(post): /sbin/ldconfig
Requires(postun): /sbin/ldconfig

%description
The Storage Performance Development Kit provides a set of tools
and libraries for writing high performance, scalable, user-mode storage
applications.


%package devel
Summary: Storage Performance Development Kit development files
Requires: %{name}%{?_isa} = %{package_version}
Provides: %{name}-static%{?_isa} = %{package_version}

%description devel
This package contains the headers and other files needed for
developing applications with the Storage Performance Development Kit.


%package tools
Summary: Storage Performance Development Kit tools files
%if "%{use_python2}" == "0"
Requires: %{name}%{?_isa} = %{package_version} python3 python3-configshell python3-pexpect
%else
Requires: %{name}%{?_isa} = %{package_version} python python-configshell pexpect
%endif
BuildArch: noarch

%description tools
%{summary}


%if %{with doc}
%package doc
Summary: Storage Performance Development Kit documentation
BuildArch: noarch

%description doc
%{summary}
%endif


%prep
# add -q
%autosetup -n spdk-%{version}


%build
./configure --prefix=%{_usr} \
	--disable-tests \
	--without-crypto \
	--with-dpdk=/usr/share/dpdk/%{config} \
	--without-fio \
	--with-vhost \
	--without-pmdk \
	--without-vpp \
	--without-rbd \
	--with-rdma \
	--with-shared \
	--with-iscsi-initiator \
	--without-vtune

make -j`nproc` all

%if %{with doc}
make -C doc
%endif

%install
%make_install -j`nproc` prefix=%{_usr} libdir=%{_libdir} datadir=%{_datadir}

# Install tools
mkdir -p %{install_datadir}
find scripts -type f -regextype egrep -regex '.*(spdkcli|rpc).*[.]py' \
	-exec cp --parents -t %{install_datadir} {} ";"

# env is banned - replace '/usr/bin/env anything' with '/usr/bin/anything'
find %{install_datadir}/scripts -type f -regextype egrep -regex '.*([.]py|[.]sh)' \
	-exec sed -i -E '1s@#!/usr/bin/env (.*)@#!/usr/bin/\1@' {} +

%if "%{use_python2}" == "1"
find %{install_datadir}/scripts -type f -regextype egrep -regex '.*([.]py)' \
	-exec sed -i -E '1s@#!/usr/bin/python3@#!/usr/bin/python2@' {} +
%endif

# synlinks to tools
mkdir -p %{install_sbindir}
ln -sf -r %{install_datadir}/scripts/rpc.py %{install_sbindir}/%{name}-rpc
ln -sf -r %{install_datadir}/scripts/spdkcli.py %{install_sbindir}/%{name}-cli

%if %{with doc}
# Install doc
mkdir -p %{install_docdir}
mv doc/output/html/ %{install_docdir}
%endif


%post -p /sbin/ldconfig
%postun -p /sbin/ldconfig


%files
%{_bindir}/spdk_*
%{_libdir}/*.so.*


%files devel
%{_includedir}/%{name}
%{_libdir}/*.a
%{_libdir}/*.so


%files tools
%{_datadir}/%{name}/scripts
%{_sbindir}/%{name}-rpc
%{_sbindir}/%{name}-cli

%if %{with doc}
%files doc
%{_docdir}/%{name}
%endif


%changelog
* Thu Nov 26 2020 Shihao Sun <sunshihao@huawei.com> - 20.01.1-2
- modify license
* Sat Nov 7 2020 Feilong Lin <linfeilong@huawei.com> - 20.01.1-1
- Support aarch64
* Tue Sep 18 2018 Pawel Wodkowski <pawelx.wodkowski@intel.com> - 0:18.07-3
- Initial RPM release
