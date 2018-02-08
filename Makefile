include config.mk

DESTDIR ?= ./out

all: libs wsupp wifi

libs: common.a crypto.a nlusctl.a netlink.a

wsupp: common.a crypto.a nlusctl.a netlink.a \
	wsupp.o wsupp_netlink.o wsupp_eapol.o wsupp_crypto.o wsupp_cntrl.o \
	wsupp_slots.o wsupp_sta_ies.o wsupp_config.o wsupp_apsel.o \
	wsupp_rfkill.o wsupp_ifmon.o

wifi: common.a crypto.a nlusctl.a \
	wifi.o wifi_dump.o wifi_pass.o wifi_wire.o

%: %.o
	$(CC) $(LDFLAGS) -o $@ $(filter %.o,$^) $(filter %.a,$^) $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f *.a *.o */*.o

install: install-bin install-man
	install -d $(DESTDIR)$(man1dir) $(DESTDIR)$(man8dir)
	install -m 0644 -t $(DESTDIR)$(man1dir) wifi.1
	install -m 0644 -t $(DESTDIR)$(man8dir) wsupp.8

install-bin:
	install -d $(DESTDIR)$(bindir) $(DESTDIR)$(sbindir)
	install -m 0755 -t $(DESTDIR)$(bindir) wifi
	install -m 0755 -t $(DESTDIR)$(sbindir) wsupp

install-man:
	mkdir -p $(man1dir) $(man8dir)

define subdir
src-$1 := $$(wildcard $1/*.c $1/*.h)
obj-$1 := $$(patsubst %.c,%.o,$$(filter %.c,$$(src-$1)))

$$(obj-$1): $$(src-$1)
	$(MAKE) -C $1

$1.a: $$(obj-$1)
	$(AR) cr $$@ $1/*.o
endef

$(eval $(call subdir,common))
$(eval $(call subdir,crypto))
$(eval $(call subdir,nlusctl))
$(eval $(call subdir,netlink))
