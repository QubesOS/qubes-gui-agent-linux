extra_cflags := $(shell pkg-config --cflags libpipewire-0.3 vchan-xen) \
  -fno-strict-aliasing -fno-strict-overflow \
  -fno-delete-null-pointer-checks -fPIC -Wall -Wextra -Werror \
  -Wno-missing-field-initializers -Wno-unused-parameter -ggdb -O2
extra_ldflags :=  $(shell pkg-config --libs libpipewire-0.3 vchan-xen) -lqubesdb
CFLAGS += $(extra_cflags)
LDLIBS += $(extra_ldflags)
LDFLAGS += -ggdb -Wl,--no-as-needed,--no-allow-shlib-undefined,-z,relro,-z,now
qubes-pw-module.so: build/qubes-pw-module.o | Makefile
	@mkdir -p -m 0700 -- build
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ -shared $(LDLIBS)

build/%.o: %.c Makefile
	@mkdir -p -m 0700 -- build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $< -MP -MD -MF $@.dep

.PHONY: clean
clean:
	rm -rf -- build
-include build/*.o.dep
