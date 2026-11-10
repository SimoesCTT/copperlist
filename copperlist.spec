Name:           copperlist
Version:        0.1.3
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
if [ -e /dev/dri/renderD128 ]; then
    make check
else
    echo "SKIP: no /dev/dri/renderD128 in buildroot; audit test requires a render node"
fi


%install
install -d %{buildroot}%{_includedir}/copperlist
install -m 0644 include/copper.h %{buildroot}%{_includedir}/copperlist/


%files
%{_includedir}/copperlist
%license LICENSE
%doc README.md
%doc docs/audit/0001-copper.md


%changelog
* Wed Nov 11 2026 Americo Simoes <amexsimoes@gmail.com> - 0.1.3-1
- %check tests for /dev/dri/renderD128 instead of the Copr macro.
  The old guard fired in Copr but not in fedora-review's mock, where
  the GPU test ran and failed. The honest condition is "can this
  buildroot run the test," not "is this Copr."

* Wed Nov 11 2026 Americo Simoes <amexsimoes@gmail.com> - 0.1.2-1
- Source0 to /archive/ (Copr cannot fetch a release asset that was
  never uploaded; the tag-only tarball resolves from the tag alone).
- Guard %check on copr_projectname (Copr mock has no render node).

* Tue Nov 10 2026 Americo Simoes <amexsimoes@gmail.com> - 0.1.0-1
- Initial package
