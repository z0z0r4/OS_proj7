K=kernel
U=user
ARCH ?= riscv

COMMON_OBJS = \
  $K/console.o \
  $K/printf.o \
  $K/uart.o \
  $K/kalloc.o \
  $K/spinlock.o \
  $K/string.o \
  $K/main.o \
  $K/syscall.o \
  $K/sysproc.o \
  $K/sleeplock.o \
  $K/file.o \
  $K/pipe.o \
  $K/exec.o \
  $K/sysfile.o \
  $K/ipc.o \
  $K/bootmod.o

ifeq ($(ARCH),riscv)
ARCH_OBJS = \
  $K/entry.o \
  $K/start.o \
  $K/vm.o \
  $K/proc.o \
  $K/swtch.o \
  $K/trampoline.o \
  $K/trap.o \
  $K/kernelvec.o \
  $K/hal.o \
  $K/plic.o \
  $K/virtio_disk.o
KERNEL_LD = $K/kernel.ld
USER_LD = $U/user.ld
ARCH_CFLAGS = -DARCH_RISCV -mcmodel=medany
ARCH_ASFLAGS =
QEMU = qemu-system-riscv64
else ifeq ($(ARCH),loongarch)
ARCH_OBJS = \
  $K/arch/loongarch/entry.o \
  $K/arch/loongarch/start.o \
  $K/arch/loongarch/vm.o \
  $K/proc.o \
  $K/arch/loongarch/swtch.o \
  $K/arch/loongarch/uservec.o \
  $K/arch/loongarch/kernelvec.o \
  $K/arch/loongarch/tlbrefill.o \
  $K/arch/loongarch/merror.o \
  $K/arch/loongarch/trap.o \
  $K/arch/loongarch/hal.o \
  $K/arch/loongarch/apic.o \
  $K/arch/loongarch/extioi.o \
  $K/arch/loongarch/virtio_pci.o \
  $K/virtio_disk.o
KERNEL_LD = $K/arch/loongarch/kernel.ld
USER_LD = $K/arch/loongarch/user.ld
ARCH_CFLAGS = -DARCH_LOONGARCH -I$K -I$K/arch/loongarch -march=loongarch64 -mabi=lp64s
ARCH_ASFLAGS = -DARCH_LOONGARCH -I$K -I$K/arch/loongarch -march=loongarch64 -mabi=lp64s
QEMU = qemu-system-loongarch64
else
$(error unsupported ARCH=$(ARCH))
endif

OBJS = $(ARCH_OBJS) $(COMMON_OBJS)
ARCH_STAMP = .arch-$(ARCH)

$(ARCH_STAMP):
	rm -f */*.o */*/*.o */*.d */*/*.d
	rm -f .arch-*
	touch $@

$(OBJS): $(ARCH_STAMP)

%.o: $(ARCH_STAMP)

# riscv64-unknown-elf- or riscv64-linux-gnu-
# perhaps in /opt/riscv/bin
#TOOLPREFIX = 

# Try to infer the correct TOOLPREFIX if not set
ifndef TOOLPREFIX
ifeq ($(ARCH),riscv)
TOOLPREFIX := $(shell if riscv64-unknown-elf-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
	then echo 'riscv64-unknown-elf-'; \
	elif riscv64-elf-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
	then echo 'riscv64-elf-'; \
	elif riscv64-linux-gnu-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
	then echo 'riscv64-linux-gnu-'; \
	elif riscv64-unknown-linux-gnu-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
	then echo 'riscv64-unknown-linux-gnu-'; \
	else echo "***" 1>&2; \
	echo "*** Error: Couldn't find a riscv64 version of GCC/binutils." 1>&2; \
	echo "*** To turn off this error, run 'gmake TOOLPREFIX= ...'." 1>&2; \
	echo "***" 1>&2; exit 1; fi)
else ifeq ($(ARCH),loongarch)
TOOLPREFIX := $(shell if loongarch64-linux-gnu-objdump -i >/dev/null 2>&1; \
	then echo 'loongarch64-linux-gnu-'; \
	elif loongarch64-unknown-linux-gnu-objdump -i >/dev/null 2>&1; \
	then echo 'loongarch64-unknown-linux-gnu-'; \
	else echo ''; fi)
endif
endif

ifeq ($(ARCH),loongarch)
ifeq ($(TOOLPREFIX),)
$(error Couldn't find a loongarch64 GCC/binutils; set TOOLPREFIX=... ARCH=loongarch)
endif
endif

MIN_QEMU_VERSION = 7.2

ifeq ($(ARCH),loongarch)
CC := $(shell if command -v $(TOOLPREFIX)gcc >/dev/null 2>&1; \
	then echo $(TOOLPREFIX)gcc; \
	elif command -v $(TOOLPREFIX)gcc-14 >/dev/null 2>&1; \
	then echo $(TOOLPREFIX)gcc-14; \
	else echo $(TOOLPREFIX)gcc; fi)
else
CC = $(TOOLPREFIX)gcc
endif
AS = $(TOOLPREFIX)gas
LD = $(TOOLPREFIX)ld
OBJCOPY = $(TOOLPREFIX)objcopy
OBJDUMP = $(TOOLPREFIX)objdump

CFLAGS = -Wall -Werror -O -fno-omit-frame-pointer -ggdb -gdwarf-2
CFLAGS += -MD
CFLAGS += $(ARCH_CFLAGS)
CFLAGS += -ffreestanding
CFLAGS += -fno-common -nostdlib
CFLAGS += -fno-builtin-strncpy -fno-builtin-strncmp -fno-builtin-strlen -fno-builtin-memset
CFLAGS += -fno-builtin-memmove -fno-builtin-memcmp -fno-builtin-log -fno-builtin-bzero
CFLAGS += -fno-builtin-strchr -fno-builtin-exit -fno-builtin-malloc -fno-builtin-putc
CFLAGS += -fno-builtin-free
CFLAGS += -fno-builtin-memcpy -Wno-main
CFLAGS += -fno-builtin-printf -fno-builtin-fprintf -fno-builtin-vprintf
CFLAGS += -I.
# Extra flags for specialized targets, e.g. KEXTRA=-DP0_TEST_HOOKS for the
# fault test build; normal builds leave KEXTRA empty.
CFLAGS += $(KEXTRA)
CFLAGS += $(shell $(CC) -fno-stack-protector -E -x c /dev/null >/dev/null 2>&1 && echo -fno-stack-protector)

# Disable PIE when possible (for Ubuntu 16.10 toolchain)
ifneq ($(shell $(CC) -dumpspecs 2>/dev/null | grep -e '[^f]no-pie'),)
CFLAGS += -fno-pie -no-pie
endif
ifneq ($(shell $(CC) -dumpspecs 2>/dev/null | grep -e '[^f]nopie'),)
CFLAGS += -fno-pie -nopie
endif

LDFLAGS = -z max-page-size=4096

$K/kernel: $(OBJS) $(KERNEL_LD)
	$(LD) $(LDFLAGS) -T $(KERNEL_LD) -o $K/kernel $(OBJS) 
	$(OBJDUMP) -S $K/kernel > $K/kernel.asm
	$(OBJDUMP) -t $K/kernel | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $K/kernel.sym

$K/%.o: $K/%.S
	$(CC) $(ARCH_ASFLAGS) -g -c -o $@ $<

tags: $(OBJS)
	etags kernel/*.S kernel/*.c

ULIB = $U/ulib.o $U/usys.o $U/printf.o $U/umalloc.o

# The file system server: the old kernel file system code, run in user mode.
F=fssrv
FSSRV_OBJS = $F/main.o $F/glue.o $F/bio.o $F/block.o $F/fs.o $F/log.o \
  $F/vfs.o $F/ext2.o $F/fat32.o

$F/%.o: $F/%.c $(ARCH_STAMP)
	$(CC) $(CFLAGS) -DFSSRV_FAULT_INJECT -I$F -I$K -c -o $@ $<

$U/_fssrv: $(FSSRV_OBJS) $(ULIB) $(USER_LD)
	$(LD) $(LDFLAGS) -T $(USER_LD) -o $@ $(FSSRV_OBJS) $(ULIB)
	$(OBJDUMP) -S $@ > $F/fssrv.asm

# the kernel embeds the server as a boot module
$K/bootmod.o: $K/bootmod.S $U/_fssrv
	$(CC) $(ARCH_ASFLAGS) -I. -g -c -o $@ $<

$(ULIB): $(ARCH_STAMP)

_%: %.o $(ULIB) $(USER_LD)
	$(LD) $(LDFLAGS) -T $(USER_LD) -o $@ $< $(ULIB)
	$(OBJDUMP) -S $@ > $*.asm
	$(OBJDUMP) -t $@ | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $*.sym

$U/usys.S : $U/usys.pl
	perl $U/usys.pl > $U/usys.S

$U/usys.o : $U/usys.S
	$(CC) $(CFLAGS) -c -o $U/usys.o $U/usys.S

$U/_forktest: $U/forktest.o $(ULIB)
	# forktest has less library code linked in - needs to be small
	# in order to be able to max out the proc table.
	$(LD) $(LDFLAGS) -N -e main -Ttext 0 -o $U/_forktest $U/forktest.o $U/ulib.o $U/usys.o
	$(OBJDUMP) -S $U/_forktest > $U/forktest.asm

mkfs/mkfs: mkfs/mkfs.c $K/fs.h $K/param.h
	gcc -I. -o mkfs/mkfs mkfs/mkfs.c

mkfs/mkfat32: mkfs/mkfat32.c
	gcc -Wall -Werror -O -o mkfs/mkfat32 mkfs/mkfat32.c

# Prevent deletion of intermediate files, e.g. cat.o, after first build, so
# that disk image changes after first build are persistent until clean.  More
# details:
# http://www.gnu.org/software/make/manual/html_node/Chained-Rules.html
.PRECIOUS: %.o

UPROGS=\
	$U/_cat\
	$U/_echo\
	$U/_forktest\
	$U/_grep\
	$U/_init\
	$U/_kill\
	$U/_ln\
	$U/_ls\
	$U/_mkdir\
	$U/_mount\
	$U/_umount\
	$U/_umountstress\
	$U/_rm\
	$U/_sh\
	$U/_stressfs\
	$U/_hello\
	$U/_test1\
	$U/_test2\
	$U/_unlinkopen\
	$U/_bigfile\
	$U/_vfsbench\
	$U/_usertests\
	$U/_grind\
	$U/_wc\
	$U/_zombie\
	$U/_logstress\
	$U/_forphan\
	$U/_dorphan\
	$U/_fsfault\

EXT2IMG = ext2.img
EXT2_BLOCKS = 4096
FAT32IMG = fat32.img

$(EXT2IMG): hello.c $K/param.h
	@command -v mkfs.ext2 >/dev/null || { echo "missing mkfs.ext2 (install e2fsprogs)"; exit 1; }
	@command -v debugfs >/dev/null || { echo "missing debugfs (install e2fsprogs)"; exit 1; }
	rm -f $(EXT2IMG)
	dd if=/dev/zero of=$(EXT2IMG) bs=1024 count=$(EXT2_BLOCKS)
	mkfs.ext2 -F -q -b 1024 -I 128 -m 0 $(EXT2IMG)
	debugfs -w -R "write hello.c hello.c" $(EXT2IMG)
	debugfs -w -R "write hello.c verylongfilename.txt" $(EXT2IMG)
	dd if=/dev/zero bs=1024 count=1024 of=hostbig.bin 2>/dev/null
	debugfs -w -R "write hostbig.bin hostbig.bin" $(EXT2IMG)
	rm -f hostbig.bin

$(FAT32IMG): mkfs/mkfat32 $K/param.h
	rm -f $(FAT32IMG)
	mkfs/mkfat32 $(FAT32IMG)

tests/test1: $U/_test1
	ln -sf ../user/_test1 $@

tests/test2: $U/_test2
	ln -sf ../user/_test2 $@

tests/hello: $U/_hello
	ln -sf ../user/_hello $@

fs.img: mkfs/mkfs README hello.c tests/cmds.sh tests/hello.c tests/test1 tests/test2 tests/hello $(UPROGS) $(EXT2IMG) $(FAT32IMG)
	mkfs/mkfs fs.img README hello.c tests/cmds.sh tests/hello.c tests/test1 tests/test2 tests/hello $(UPROGS)
	cat $(EXT2IMG) >> fs.img
	cat $(FAT32IMG) >> fs.img

-include kernel/*.d kernel/arch/loongarch/*.d user/*.d fssrv/*.d

clean: 
	rm -f *.tex *.dvi *.idx *.aux *.log *.ind *.ilg \
	*/*.o */*/*.o */*/*/*.o */*.d */*/*.d */*/*/*.d \
	*/*.asm */*/*.asm */*/*/*.asm */*.sym */*/*.sym */*/*/*.sym \
	$K/kernel fs.img \
	.arch-* \
	$(EXT2IMG) \
	$(FAT32IMG) \
	mkfs/mkfs mkfs/mkfat32 .gdbinit \
	tests/test1 tests/test2 tests/hello \
        $U/usys.S \
	$(UPROGS)

# try to generate a unique GDB port
GDBPORT = $(shell expr `id -u` % 5000 + 25000)
# QEMU's gdb stub command line changed in 0.11
QEMUGDB = $(shell if $(QEMU) -help | grep -q '^-gdb'; \
	then echo "-gdb tcp::$(GDBPORT)"; \
	else echo "-s -p $(GDBPORT)"; fi)
ifndef CPUS
CPUS := 3
endif

ifeq ($(ARCH),loongarch)
QEMUOPTS = -machine virt -kernel $K/kernel -m 1G -smp $(CPUS) -nographic -no-reboot -net none
QEMUOPTS += -drive file=fs.img,if=none,format=raw,id=x0
QEMUOPTS += -device virtio-blk-pci,drive=x0,disable-legacy=on,vectors=0
else
QEMUOPTS = -machine virt -bios none -kernel $K/kernel -m 128M -smp $(CPUS) -nographic
QEMUOPTS += -global virtio-mmio.force-legacy=false
QEMUOPTS += -drive file=fs.img,if=none,format=raw,id=x0
QEMUOPTS += -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
endif

qemu: check-qemu-version $K/kernel fs.img
	$(QEMU) $(QEMUOPTS)

.gdbinit: .gdbinit.tmpl-riscv
	sed "s/:1234/:$(GDBPORT)/" < $^ > $@

qemu-gdb: $K/kernel .gdbinit fs.img
	@echo "*** Now run 'gdb' in another window." 1>&2
	$(QEMU) $(QEMUOPTS) -S $(QEMUGDB)

print-gdbport:
	@echo $(GDBPORT)

QEMU_VERSION := $(shell $(QEMU) --version | head -n 1 | sed -E 's/^QEMU emulator version ([0-9]+\.[0-9]+)\..*/\1/')
check-qemu-version:
	@if [ "$(shell echo "$(QEMU_VERSION) >= $(MIN_QEMU_VERSION)" | bc)" -eq 0 ]; then \
		echo "ERROR: Need qemu version >= $(MIN_QEMU_VERSION)"; \
		exit 1; \
	fi
