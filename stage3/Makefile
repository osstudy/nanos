LWIPDIR=	$(VENDORDIR)/lwip

PROGRAMS=	stage3.img
SRCS-stage3.img= \
	$(CURDIR)/stage3.c \
	$(OBJDIR)/gitversion.c \
	$(SRCDIR)/drivers/ata.c \
	$(SRCDIR)/drivers/ata-pci.c \
	$(SRCDIR)/drivers/console.c \
	$(SRCDIR)/drivers/storage.c \
	$(SRCDIR)/drivers/vga.c \
	$(SRCDIR)/gdb/gdbstub.c \
	$(SRCDIR)/gdb/gdbtcp.c \
	$(SRCDIR)/gdb/gdbutil.c \
	$(SRCDIR)/net/net.c \
	$(SRCDIR)/net/netsyscall.c \
	$(SRCDIR)/runtime/bitmap.c \
	$(SRCDIR)/runtime/buffer.c \
	$(SRCDIR)/runtime/extra_prints.c \
	$(SRCDIR)/runtime/format.c \
	$(SRCDIR)/runtime/heap/freelist.c \
	$(SRCDIR)/runtime/heap/id.c \
	$(SRCDIR)/runtime/heap/mcache.c \
	$(SRCDIR)/runtime/heap/objcache.c \
	$(SRCDIR)/runtime/memops.c \
	$(SRCDIR)/runtime/merge.c \
	$(SRCDIR)/runtime/pqueue.c \
	$(SRCDIR)/runtime/random.c \
	$(SRCDIR)/runtime/range.c \
	$(SRCDIR)/runtime/runtime_init.c \
	$(SRCDIR)/runtime/sha256.c \
	$(SRCDIR)/runtime/symbol.c \
	$(SRCDIR)/runtime/table.c \
	$(SRCDIR)/runtime/timer.c \
	$(SRCDIR)/runtime/tuple.c \
	$(SRCDIR)/runtime/string.c \
	$(SRCDIR)/runtime/crypto/chacha.c \
	$(SRCDIR)/tfs/tfs.c \
	$(SRCDIR)/tfs/tlog.c \
	$(SRCDIR)/unix/blockq.c \
	$(SRCDIR)/unix/exec.c \
	$(SRCDIR)/unix/eventfd.c \
	$(SRCDIR)/unix/mktime.c \
	$(SRCDIR)/unix/mmap.c \
	$(SRCDIR)/unix/notify.c \
	$(SRCDIR)/unix/poll.c \
	$(SRCDIR)/unix/signal.c \
	$(SRCDIR)/unix/socketpair.c \
	$(SRCDIR)/unix/special.c \
	$(SRCDIR)/unix/syscall.c \
	$(SRCDIR)/unix/thread.c \
	$(SRCDIR)/unix/unix_clock.c \
	$(SRCDIR)/unix/unix.c \
	$(SRCDIR)/unix/vdso.c \
	$(SRCDIR)/unix/pipe.c \
	$(SRCDIR)/virtio/virtio_net.c \
	$(SRCDIR)/virtio/virtio_pci.c \
	$(SRCDIR)/virtio/virtio_storage.c \
	$(SRCDIR)/virtio/virtio_scsi.c \
	$(SRCDIR)/virtio/virtqueue.c \
	$(SRCDIR)/virtio/scsi.c \
	$(SRCDIR)/x86_64/backed_heap.c \
	$(SRCDIR)/x86_64/breakpoint.c \
	$(SRCDIR)/x86_64/clock.c \
	$(SRCDIR)/x86_64/crt0.s \
	$(SRCDIR)/x86_64/elf.c \
	$(SRCDIR)/x86_64/hpet.c \
	$(SRCDIR)/x86_64/interrupt.c \
	$(SRCDIR)/x86_64/kvm_platform.c \
	$(SRCDIR)/x86_64/page.c \
	$(SRCDIR)/x86_64/pci.c \
	$(SRCDIR)/x86_64/queue.c \
	$(SRCDIR)/x86_64/rtc.c \
	$(SRCDIR)/x86_64/serial.c \
	$(SRCDIR)/x86_64/service.c \
	$(SRCDIR)/x86_64/symtab.c \
	$(SRCDIR)/x86_64/synth.c \
	$(SRCS-lwip)
SRCS-lwip= \
	$(LWIPDIR)/src/core/def.c \
	$(LWIPDIR)/src/core/inet_chksum.c \
	$(LWIPDIR)/src/core/init.c \
	$(LWIPDIR)/src/core/ip.c \
	$(LWIPDIR)/src/core/ipv4/dhcp.c \
	$(LWIPDIR)/src/core/ipv4/etharp.c \
	$(LWIPDIR)/src/core/ipv4/icmp.c \
	$(LWIPDIR)/src/core/ipv4/ip4_addr.c \
	$(LWIPDIR)/src/core/ipv4/ip4_frag.c \
	$(LWIPDIR)/src/core/ipv4/ip4.c \
	$(LWIPDIR)/src/core/mem.c \
	$(LWIPDIR)/src/core/memp.c \
	$(LWIPDIR)/src/core/netif.c \
	$(LWIPDIR)/src/core/pbuf.c \
	$(LWIPDIR)/src/core/stats.c \
	$(LWIPDIR)/src/core/tcp_in.c \
	$(LWIPDIR)/src/core/tcp_out.c \
	$(LWIPDIR)/src/core/tcp.c \
	$(LWIPDIR)/src/core/timeouts.c \
	$(LWIPDIR)/src/core/udp.c \
	$(LWIPDIR)/src/api/err.c \
	$(LWIPDIR)/src/netif/ethernet.c
CFLAGS+=	$(KERNCFLAGS) -DSTAGE3
CFLAGS+= \
	-I$(SRCDIR) \
	-I$(SRCDIR)/gdb \
	-I$(SRCDIR)/net \
	-I$(SRCDIR)/runtime \
	-I$(SRCDIR)/tfs \
	-I$(SRCDIR)/unix \
	-I$(SRCDIR)/x86_64 \
	-I$(LWIPDIR)/src/include
#CFLAGS+=	-DLWIPDIR_DEBUG -DEPOLL_DEBUG -DNETSYSCALL_DEBUG -DKERNEL_DEBUG
AFLAGS+=	-felf64 -I$(OBJDIR)/
LDFLAGS+=	$(KERNLDFLAGS) -T linker_script

CLEANFILES+=	$(foreach f,gitversion.c frame.inc stage3.dis stage3.img,$(OBJDIR)/$f)
CLEANDIRS+=	$(foreach d,output src vendor vendor/lwip vendor/lwip/src,$(OBJDIR)/$d)

OBJDUMPFLAGS=	-d -S -M intel-mnemonic
GITFLAGS+=	--depth 1 http://git.savannah.nongnu.org/git/lwip.git -b STABLE-2_0_3_RELEASE

msg_objdump=	OBJDUMP	$@
cmd_objdump=	$(OBJDUMP) $(OBJDUMPFLAGS) $(OBJDUMPFLAGS_$(@F)) $< $< >$@

msg_sed=	SED	$@
cmd_sed=	$(SED) -e 's/\#/%/' <$^ >$@

msg_version=	VERSION	$@
cmd_version=	$(ECHO) "const char *gitversion = \"$(shell $(GIT) rev-parse HEAD)\";" >$@

all: stage3.dis

include ../rules.mk

.PHONY: stage3.dis

stage3.dis: $(OBJDIR)/stage3.dis

$(OBJDIR)/stage3.dis: $(PROG-stage3.img)
	$(call cmd,objdump)

$(PROG-stage3.img): linker_script

$(OBJDIR)/gitversion.c: $(ROOTDIR)/.git/index $(ROOTDIR)/.git/HEAD
	$(call cmd,version)

$(OBJS-stage3.img): $(LWIPDIR)/.vendored

$(OBJDIR)/src/x86_64/crt0.o: $(OBJDIR)/frame.inc

$(OBJDIR)/frame.inc: $(SRCDIR)/x86_64/frame.h
	$(call cmd,sed)

ifeq ($(UNAME_s),Darwin)
CFLAGS+=	-target x86_64-elf
LD=		x86_64-elf-ld
OBJDUMP=	x86_64-elf-objdump
else
LD=		ld
endif
