CFLAGS += $(shell pkg-config --cflags libpipewire-0.3 vchan-xen) \
  -fno-strict-aliasing -fno-strict-overflow \
  -fno-delete-null-pointer-checks -fPIC -Wall -Wextra -Werror \
  -Wno-missing-field-initializers -Wno-unused-parameter -ggdb -O2 \
  -D_FORTIFY_SOURCE=2
LDLIBS += $(LDFLAGS) $(shell pkg-config --libs libpipewire-0.3 vchan-xen) -lqubesdb -ggdb

qubes-pw-module.so: build/qubes-pw-module.o | GNUmakefile
	@mkdir -p -m 0700 -- build
	$(CC) $(CFLAGS) $(LDLIBS) -o $@ $^ -shared -Wl,--no-allow-shlib-undefined,-z,relro,-z,now

build/%.o: %.c GNUmakefile
	@mkdir -p -m 0700 -- build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $< -MP -MD -MF $@.dep

.PHONY: clean
clean:
	rm -rf -- build
-include build/*.o.dep
