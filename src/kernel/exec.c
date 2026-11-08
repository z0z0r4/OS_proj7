#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"
#include "ipc.h"
#include "fcntl.h"
#include "stat.h"

// Where an executable is read from: a file held by the file system
// server, or an in-memory boot module (used to start the server itself).
struct execsrc {
  int h, gen;                 // server handle
  char *image;                // boot module, if non-zero
  uint64 imagesz;
};

static int loadseg(pde_t *, uint64, struct execsrc *, uint, uint);

static int
srcread(struct execsrc *src, void *dst, uint off, uint n)
{
  struct ipcmsg m;
  long r;

  if(src->image){
    if(off >= src->imagesz)
      return 0;
    if(n > src->imagesz - off)
      n = src->imagesz - off;
    memmove(dst, src->image + off, n);
    return n;
  }
  memset(&m, 0, sizeof(m));
  m.op = FS_PREAD;
  m.h = src->h;
  m.gen = src->gen;
  m.a0 = off;
  m.a1 = n;
  r = fs_call(&m, (uint64)dst, n, 1, 0);
  return r;
}

// map ELF permissions to PTE permission bits.
int flags2perm(int flags)
{
    int perm = 0;
    if(flags & 0x1)
      perm = PTE_X;
    if(flags & 0x2)
      perm |= PTE_W;
    return perm;
}

//
// the implementation of the exec() system call
//
static int kexec_src(char *path, char **argv, struct execsrc *src);

int
kexec(char *path, char **argv)
{
  struct execsrc src;
  struct ipcmsg m;
  char rootpath[MAXPATH];
  long h;
  int i, slash, ret;

  memset(&src, 0, sizeof(src));
  memset(&m, 0, sizeof(m));
  m.op = FS_OPEN;
  safestrcpy(m.s1, path, sizeof(m.s1));
  m.a0 = O_RDONLY;
  h = fs_call(&m, 0, 0, 0, &src.gen);
  if(h <= 0 && path[0] != '/'){
    // Bare command names (no '/') fall back to the root directory,
    // so programs installed in "/" work from any working directory.
    slash = 0;
    for(i = 0; path[i]; i++)
      if(path[i] == '/')
        slash = 1;
    if(!slash && strlen(path) + 2 <= sizeof(rootpath)){
      rootpath[0] = '/';
      safestrcpy(rootpath + 1, path, sizeof(rootpath) - 1);
      memset(&m, 0, sizeof(m));
      m.op = FS_OPEN;
      safestrcpy(m.s1, rootpath, sizeof(m.s1));
      m.a0 = O_RDONLY;
      h = fs_call(&m, 0, 0, 0, &src.gen);
    }
  }
  if(h <= 0)
    return -1;
  src.h = h;
  ret = kexec_src(path, argv, &src);
  fs_release(src.h, src.gen);
  return ret;
}

// exec from a boot module that is linked into the kernel image.
int
kexec_image(char *name, char *image, uint64 size, char **argv)
{
  struct execsrc src;

  memset(&src, 0, sizeof(src));
  src.image = image;
  src.imagesz = size;
  return kexec_src(name, argv, &src);
}

static int
kexec_src(char *path, char **argv, struct execsrc *src)
{
  char *s, *last;
  int i, off;
  uint64 argc, sz = 0, sp, ustack[MAXARG], stackbase;
  struct elfhdr elf;
  struct proghdr ph;
  pagetable_t pagetable = 0, oldpagetable;
  struct proc *p = myproc();

  // Read the ELF header.
  if(srcread(src, &elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;

  // Is this really an ELF file?
  if(elf.magic != ELF_MAGIC)
    goto bad;

  if((pagetable = proc_pagetable(p)) == 0)
    goto bad;

  // Load program into memory.
  for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
    if(srcread(src, &ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if(ph.type != ELF_PROG_LOAD)
      continue;
    if(ph.memsz < ph.filesz)
      goto bad;
    if(ph.vaddr + ph.memsz < ph.vaddr)
      goto bad;
    if(ph.vaddr % PGSIZE != 0)
      goto bad;
    uint64 sz1;
    if((sz1 = uvmalloc(pagetable, sz, ph.vaddr + ph.memsz, flags2perm(ph.flags))) == 0)
      goto bad;
    sz = sz1;
    if(loadseg(pagetable, ph.vaddr, src, ph.off, ph.filesz) < 0)
      goto bad;
  }
  p = myproc();
  uint64 oldsz = p->sz;

  // Allocate some pages at the next page boundary.
  // Make the first inaccessible as a stack guard.
  // Use the rest as the user stack.
  sz = PGROUNDUP(sz);
  uint64 sz1;
  if((sz1 = uvmalloc(pagetable, sz, sz + (USERSTACK+1)*PGSIZE, PTE_W)) == 0)
    goto bad;
  sz = sz1;
  uvmclear(pagetable, sz-(USERSTACK+1)*PGSIZE);
  sp = sz;
  stackbase = sp - USERSTACK*PGSIZE;

  // Copy argument strings into new stack, remember their
  // addresses in ustack[].
  for(argc = 0; argv[argc]; argc++) {
    if(argc >= MAXARG)
      goto bad;
    sp -= strlen(argv[argc]) + 1;
    sp -= sp % 16; // riscv sp must be 16-byte aligned
    if(sp < stackbase)
      goto bad;
    if(copyout(pagetable, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    ustack[argc] = sp;
  }
  ustack[argc] = 0;

  // push a copy of ustack[], the array of argv[] pointers.
  sp -= (argc+1) * sizeof(uint64);
  sp -= sp % 16;
  if(sp < stackbase)
    goto bad;
  if(copyout(pagetable, sp, (char *)ustack, (argc+1)*sizeof(uint64)) < 0)
    goto bad;

  // a0 and a1 contain arguments to user main(argc, argv)
  // argc is returned via the system call return
  // value, which goes in a0.
  p->trapframe->a1 = sp;

  // Save program name for debugging.
  for(last=s=path; *s; s++)
    if(*s == '/')
      last = s+1;
  safestrcpy(p->name, last, sizeof(p->name));
    
  // Commit to the user image.
  oldpagetable = p->pagetable;
  p->pagetable = pagetable;
  p->sz = sz;
#ifdef ARCH_LOONGARCH
  p->trapframe->era = elf.entry;  // initial program counter = main
#else
  p->trapframe->epc = elf.entry;  // initial program counter = main
#endif
  p->trapframe->sp = sp; // initial stack pointer
  proc_freepagetable(oldpagetable, oldsz);

  return argc; // this ends up in a0, the first argument to main(argc, argv)

 bad:
  if(pagetable)
    proc_freepagetable(pagetable, sz);
  return -1;
}

// Load an ELF program segment into pagetable at virtual address va.
// va must be page-aligned
// and the pages from va to va+sz must already be mapped.
// Returns 0 on success, -1 on failure.
static int
loadseg(pagetable_t pagetable, uint64 va, struct execsrc *src, uint offset, uint sz)
{
  uint i, n;
  uint64 pa;

  for(i = 0; i < sz; i += PGSIZE){
    pa = walkaddr(pagetable, va + i);
    if(pa == 0)
      panic("loadseg: address should exist");
    if(sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
#ifdef ARCH_LOONGARCH
    pa |= DMWIN_MASK;
#endif
    if(srcread(src, (void *)pa, offset+i, n) != n)
      return -1;
  }
  
  return 0;
}
