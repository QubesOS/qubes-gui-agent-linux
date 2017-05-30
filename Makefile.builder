ifeq ($(PACKAGE_SET),vm)
RPM_SPEC_FILES := rpm_spec/gui-agent.spec
ARCH_BUILD_DIRS := archlinux
DEBIAN_BUILD_DIRS := debian

ifneq (,$(findstring $(DIST),xenial))
SOURCE_COPY_IN := source-debian-quilt-copy-in
source-debian-quilt-copy-in:
	-$(shell $(ORIG_SRC)/debian-quilt $(ORIG_SRC)/series-debian-vm.conf $(CHROOT_DIR)/$(DIST_SRC)/debian/patches)
endif
endif
