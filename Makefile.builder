ifeq ($(PACKAGE_SET),vm)
RPM_SPEC_FILES := rpm_spec/gui-vm.spec
ARCH_BUILD_DIRS := archlinux
DEBIAN_BUILD_DIRS := debian
endif
