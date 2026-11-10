Name:           copperlist
Version:        0.1.0
Release:        1%{?dist}
%global debug_package %{nil}
Summary:        Copper-style trigger-list sequencer for the i915 BLT ring

License:        MIT
URL:            https://github.com/SimoesCTT/copperlist
Source0:        %{url}/archive/v%{version}/%{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  pkgconfig(libdrm)
BuildRequires:  opencl-headers
BuildRequires:  OpenCL-ICD-Loader-devel

%description
copperlist chains N blit copies into one batch GEM and one EXECBUFFER2
on the Intel i915 BLT ring, replacing the status quo's one-batch-per-link.
It is the Amiga Copper's model — a list executed by the engine, not
re-armed by userspace per step — applied to the AI pipeline layer.

Measured on Intel Skylake-U (HD Graphics 520, i915, kernel 7.2.13):
2.59x to 4.99x faster per link at N=8, 4.77x to 5.62x at N=16, across
independent 50-run batches, 0 data mismatches. Scope: Gen8/Gen9,
minterm 0xF0, bpp 4, N<=32, terminal-only completion. Not a GPU
speedup; one submit per chain instead of one per link.

Built on thelevel's chipmem and blit, vendored byte-identical.


%prep
%autosetup


%build
%make_build


%check
make check


%install
install -d %{buildroot}%{_includedir}/copperlist
install -m 0644 include/copper.h %{buildroot}%{_includedir}/copperlist/
install -d %{buildroot}%{_libdir}
install -m 0644 libcopper.a %{buildroot}%{_libdir}/


%files
%license LICENSE
%doc README.md
%doc docs/audit/0001-copper.md
%{_includedir}/copperlist
%{_libdir}/libcopper.a


%changelog
* Tue Nov 11 2026 Americo Simoes <amexsimoes@gmail.com> - 0.1.0-1
- Initial package
