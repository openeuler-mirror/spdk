# Build documentation package
%bcond_with doc

Name: spdk
Version: 21.01.1
Release: 6
Summary: Set of libraries and utilities for high performance user-mode storage
License: BSD and MIT
URL: http://spdk.io
Source0: https://github.com/spdk/spdk/archive/refs/tags/v%{version}.tar.gz
Patch1: 0001-blobstore-fix-memleak-problem-in-blob_load_cpl.patch
Patch2: 0002-blobstore-fix-potential-memleak-problem-in-blob_seri.patch
Patch3: 0003-idxd-fix-memleak-problem-in-spdk_idxd_configure_chan.patch
Patch4: 0004-idxd-fix-one-memleak-problem-in-spdk_idxd_get_channe.patch
Patch5: 0005-ioat-fix-potential-double-free-problem-in-ioat_chann.patch
Patch6: 0006-nvmf-check-return-value-of-strdup-in-spdk_nvmf_subsy.patch
Patch7: 0007-nvmf-check-return-value-of-strdup-in-spdk_nvmf_subsy.patch
Patch8: 0008-nvmf-fix-fd-leakage-problem-in-nvmf_vfio_user_listen.patch
Patch9: 0009-posix-set-fd-to-1-after-close-fd-in-posix_sock_creat.patch
Patch10: 0010-spdk_top-check-return-value-of-strdup-in-store_last_.patch
Patch11: 0011-uring-set-fd-to-1-after-close-fd-in-uring_sock_creat.patch
Patch12: 0012-spdk-use-fstack-protector-strong-instead-of-fstack-p.patch
Patch13: 0013-lib-vhost-Fix-compilation-with-dpdk-21.11.patch
Patch14: 0014-mk-Fix-debug-build-error-on-ARM-ThunderX2-and-neoverse_N1_platform.patch
Patch15: 0015-configure-add-gcc-version-check-for-ARM-Neoverse-N1_platform.patch
Patch16: 0016-Enhance-security-for-share-library.patch

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
BuildRequires: CUnit, CUnit-devel
%if %{with doc}
BuildRequires: doxygen mscgen graphviz
%endif

# Install dependencies
Requires: dpdk >= 21.11, numactl-libs, openssl-libs
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
	--without-crypto \
	--without-isal \
	--with-dpdk=/usr/lib64/dpdk/pmds-22.0 \
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

%check
sed -i "s/sudo//g" test/common/autotest_common.sh
sed -i '/target_space=/aexport SPDK_TEST_STORAGE=$target_dir;return 0' test/common/autotest_common.sh
test/unit/unittest.sh

%install
%make_install -j`nproc` prefix=%{_usr} libdir=%{_libdir} datadir=%{_datadir}
install -d $RPM_BUILD_ROOT/opt/spdk/scripts
install -d $RPM_BUILD_ROOT/opt/spdk/include/spdk
install -m 0744 ./scripts/setup.sh $RPM_BUILD_ROOT/opt/spdk/scripts/setup.sh
install -m 0744 ./scripts/common.sh $RPM_BUILD_ROOT/opt/spdk/scripts/common.sh
install -m 0644 ./include/spdk/pci_ids.h $RPM_BUILD_ROOT/opt/spdk/include/spdk/pci_ids.h

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
%dir /opt/spdk
%dir /opt/spdk/scripts
%dir /opt/spdk/include
%dir /opt/spdk/include/spdk
/opt/spdk/scripts/setup.sh
/opt/spdk/scripts/common.sh
/opt/spdk/include/spdk/pci_ids.h


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
* Sat Nov 5 2022 Weifeng Su <suweifeng1@huawei.com> - 21.01.1-6
- Enable unittest

* Mon Oct 24 2022 Hongtao Zhang <zhanghongtao22@huawei.com> - 21.01.1-5
- Add the setup.sh script during installation

* Tue Mar 15 2022 Weifeng Su <suweifeng1@huawei.com> - 21.01.1-4
- Remove rpath link option, Due to it's easy for attacher to
  construct 'rpath' attacks

* Fri Feb 25 2022 Hongtao Zhang <zhanghongtao22@huawei.com> - 21.01.1-3
- Fix build error on ARM ThunderX2 and neoverse N1 platform

* Mon Jan 10 2022 Weifeng Su <suweifeng1@huawei.com> - 21.01.1-2
- Adapt for dpdk 21.11

* Tue Nov 23 2021 Weifeng Su <suweifeng1@huawei.com> - 21.01.1-1
- rebase to v21.01.1 Maintenance LTS Version

* Mon Sep 13 2021 Zhiqiang Liu <liuzhiqiang26@huawei.com> - 21.01-5
- use -fstack-protector-strong instead of -fstack-protector for
stronger security.

* Sat Jul 24 2021 Zhiqiang Liu <liuzhiqiang26@huawei.com> - 21.01-4
- backport 13 bugfix from upstream

* Tue Jul 13 2021 Xiaokeng Li <lixiaokeng@huawei.com> - 21.01-3
- backport bugfix from upstream

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
