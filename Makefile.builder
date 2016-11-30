ifeq ($(PACKAGE_SET),vm)
RPM_SPEC_FILES := rpm_spec/gui-vm.spec
ARCH_BUILD_DIRS := archlinux
DEBIAN_BUILD_DIRS := debian
ifneq ($(filter $(DISTRIBUTION), debian qubuntu),)
SOURCE_COPY_IN := source-debian-quilt-copy-in

source-debian-quilt-copy-in: VERSION = $(shell cat $(ORIG_SRC)/version)
source-debian-quilt-copy-in: ORIG_FILE = "$(CHROOT_DIR)/$(DIST_SRC)/../qubes-gui-agent_$(VERSION).orig.tar.gz"
ifneq (,$(findstring $(DIST),xenial))
source-debian-quilt-copy-in:
	-$(shell $(ORIG_SRC)/debian-quilt $(ORIG_SRC)/series-debian-vm.conf $(CHROOT_DIR)/$(DIST_SRC)/debian/patches)
	tar cfz $(ORIG_FILE) --exclude-vcs --exclude=rpm --exclude=pkgs --exclude=deb --exclude=debian -C $(CHROOT_DIR)/$(DIST_SRC) .
else
source-debian-quilt-copy-in:
	tar cfz $(ORIG_FILE) --exclude-vcs --exclude=rpm --exclude=pkgs --exclude=deb --exclude=debian -C $(CHROOT_DIR)/$(DIST_SRC) .
endif
endif
endif
