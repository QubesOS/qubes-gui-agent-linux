ifeq ($(PACKAGE_SET),vm)
  RPM_SPEC_FILES := rpm_spec/gui-agent.spec
  ARCH_BUILD_DIRS := archlinux
  DEBIAN_BUILD_DIRS := debian

  ifneq (,$(findstring $(DISTRIBUTION),qubuntu))
    SOURCE_COPY_IN := source-debian-quilt-copy-in
  endif
endif


source-debian-quilt-copy-in:
	sed -i 's/$$(PA_VER)/1:$$(PA_VER)/g' $(CHROOT_DIR)/$(DIST_SRC)/debian/rules
	sed -i /Trolltech.conf/d $(CHROOT_DIR)/$(DIST_SRC)/debian/qubes-gui-agent.install
	-$(shell $(ORIG_SRC)/debian-quilt $(ORIG_SRC)/series-debian-vm.conf $(CHROOT_DIR)/$(DIST_SRC)/debian/patches)

# vim: filetype=make
