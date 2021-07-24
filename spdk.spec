# Build documentation package
%bcond_with doc
%global __python %{__python3}

Name: spdk
Version: 21.01
Release: 5
Summary: Set of libraries and utilities for high performance user-mode storage
License: BSD and MIT
URL: http://spdk.io
Source0: https://github.com/spdk/spdk/archive/v%{version}.tar.gz
Patch1:  0001-lib-env_dpdk-fix-the-enum-rte_kernel_driver-definiti.patch
Patch2:  0002-env_dpdk-add-rte_ethdev-dependency.patch
Patch3:  0003-pkg-spdk.spec-Add-ncurses-devel-to-BuildRequires.patch
Patch4:  0004-lib-vhost-Add-version-check-when-use-RTE_VHOST_USER_.patch
Patch5:  0005-lib-nvme-Remove-qpair-from-all-lists-before-freeing-.patch
Patch6:  0006-lib-env_dpdk-add-rte_net-dependency.patch
Patch7:  0007-pkg-add-python3-requires-in-spdk.spec.patch
Patch8:  0008-sock-add-enable_quickack-and-enable_placement_id-whe.patch
Patch9:  0009-bdev-ocssd-Fix-the-bug-that-no-media-event-is-pushed.patch
Patch10: 0010-lib-iscsi-return-immediately-from-iscsi_parse_params.patch
Patch11: 0011-nbd-set-io-timeout.patch
Patch12: 0012-lib-util-Fix-valgrind-error-reported-on-ARM-platform.patch
Patch13: 0013-lib-vhost-force-cpumask-to-be-subset-of-application-.patch
Patch14: 0014-autorun-allow-pass-configuration-file-path.patch
Patch15: 0015-spdk_top-fix-app-crashing-on-tab-selection-with-TAB-.patch
Patch16: 0016-blobfs-check-return-value-of-strdup-in-blobfs_fuse_s.patch
Patch17: 0017-blobfs-check-return-value-of-strdup-in-spdk_fs_creat.patch
Patch18: 0018-blobstore-fix-memleak-problem-in-blob_load_cpl.patch
Patch19: 0019-blobstore-fix-potential-memleak-problem-in-blob_seri.patch
Patch20: 0020-idxd-fix-memleak-problem-in-spdk_idxd_configure_chan.patch
Patch21: 0021-idxd-fix-one-memleak-problem-in-spdk_idxd_get_channe.patch
Patch22: 0022-ioat-fix-potential-double-free-problem-in-ioat_chann.patch
Patch23: 0023-nvmf-check-return-value-of-strdup-in-spdk_nvmf_subsy.patch
Patch24: 0024-nvmf-check-return-value-of-strdup-in-spdk_nvmf_subsy.patch
Patch25: 0025-nvmf-fix-fd-leakage-problem-in-nvmf_vfio_user_listen.patch
Patch26: 0026-posix-set-fd-to-1-after-close-fd-in-posix_sock_creat.patch
Patch27: 0027-spdk_top-check-return-value-of-strdup-in-store_last_.patch
Patch28: 0028-uring-set-fd-to-1-after-close-fd-in-uring_sock_creat.patch

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
BuildRequires: dpdk-devel, numactl-devel, ncurses-devel
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
Requires: dpdk >= 19.11, numactl-libs, openssl-libs
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
%autosetup -n spdk-%{version} -p1


%build
./configure --prefix=%{_usr} \
	--disable-tests \
	--disable-unit-tests \
	--without-crypto \
	--without-isal \
	--with-dpdk=/usr/share/dpdk/%{config} \
	--without-fio \
	--with-vhost \
	--without-pmdk \
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
* Sat Jul 24 2021 Zhiqiang Liu <liuzhiqiang26@huawei.com> - 21.01-5
- backport 13 bugfix from upstream

* Thu Jul 13 2021 Xiaokeng Li <lixiaokeng@huawei.com> - 21.01-4
- backport bugfix from upstream

* Wed Mar 29 2021 jeffery.Gao <gaojianxing@huawei.com> - 21.01-3
- set __python use python3 to avoid rpm build failed.

* Wed Mar 10 2021 Shihao Sun <sunshihao@huawei.com> - 21.01-2
- use --without-isal to avoid build failed in arm.

* Thu Feb 4 2021 Shihao Sun <sunshihao@huawei.com> - 21.01-1
- update spdk to 21.01 LTS version. 
* Thu Nov 26 2020 Shihao Sun <sunshihao@huawei.com> - 20.01.1-2
- modify license
* Sat Nov 7 2020 Feilong Lin <linfeilong@huawei.com> - 20.01.1-1
- Support aarch64
* Tue Sep 18 2018 Pawel Wodkowski <pawelx.wodkowski@intel.com> - 0:18.07-3
- Initial RPM release
