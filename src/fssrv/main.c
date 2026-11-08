// The file system server.
//
// Everything that used to be the kernel's file system (buffer cache, log,
// xv6/ext2/fat32 back ends, VFS, path lookup, open-file offsets) runs here
// as an ordinary user process.  It serves one request at a time, received
// over the kernel's synchronous IPC, and reaches the disk only through the
// kernel's block driver (blk_rw).  If it dies, the kernel fails the
// requests in flight and init starts a new one; the on-disk log recovers
// whatever transaction was cut short.
#include "types.h"
#include "param.h"
#include "fs.h"
#include "stat.h"
#include "fcntl.h"
#include "ipc.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "defs.h"
#include "file.h"
#include "svcapi.h"
#ifdef P0_TEST_HOOKS
#include "syscall.h"
#endif

// A handle names one reference to an inode held on behalf of a client:
// an open file (with its offset) or a current directory.
#define NFH 256

static struct fh {
  struct inode *ip;
  uint off;
} fh[NFH];

static struct ipcmsg m;

static int
halloc(struct inode *ip, uint off)
{
  int i;
  for(i = 1; i < NFH; i++){
    if(fh[i].ip == 0){
      fh[i].ip = ip;
      fh[i].off = off;
      return i;
    }
  }
  return 0;
}

static struct fh*
hget(int h)
{
  if(h <= 0 || h >= NFH || fh[h].ip == 0)
    return 0;
  return &fh[h];
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.inum != 0)
      return 0;
  }
  return 1;
}

static long
do_link(char *old, char *new)
{
  char name[DIRSIZ];
  struct inode *dp, *ip;
  int r;

  begin_op();
  if((ip = namei(old)) == 0){
    end_op();
    return -1;
  }

  if((dp = nameiparent(new, name)) == 0){
    iput(ip);
    end_op();
    return -1;
  }
  r = vfs_link(ip, dp, name);
  iput(dp);
  iput(ip);

  end_op();
  return r;
}

static long
do_unlink(char *path)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ];
  uint off;

  begin_op();
  if((dp = nameiparent(path, name)) == 0){
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if(dp->fstype != FS_XV6){
    if(vfs_unlink(dp, name) < 0)
      goto bad;
    iunlockput(dp);
    end_op();
    return 0;
  }

  if((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if(vfs_mountpoint(ip) != 0){
    // Cannot unlink a directory that is used as a mount point.
    iunlockput(ip);
    goto bad;
  }

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

// Returns a locked inode.  Must be called inside a transaction.
static struct inode*
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if((dp = nameiparent(path, name)) == 0)
    return 0;

  ilock(dp);

  if((ip = vfs_lookup(dp, name)) != 0){
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
      return ip;
    iunlockput(ip);
    return 0;
  }

  if(dp->fstype != FS_XV6){
    ip = vfs_create(dp, name, type, major, minor);
    iunlockput(dp);
    if(ip)
      ilock(ip);
    return ip;
  }

  if((ip = ialloc(dp->dev, type)) == 0){
    iunlockput(dp);
    return 0;
  }

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      goto fail;
  }

  if(dirlink(dp, name, ip->inum) < 0)
    goto fail;

  if(type == T_DIR){
    // now that success is guaranteed:
    dp->nlink++;  // for ".."
    iupdate(dp);
  }

  iunlockput(dp);

  return ip;

 fail:
  // something went wrong. de-allocate ip.
  ip->nlink = 0;
  iupdate(ip);
  iunlockput(ip);
  iunlockput(dp);
  return 0;
}

static long
do_open(struct ipcmsg *r)
{
  struct inode *ip;
  int omode = r->a0, h;

  begin_op();

  if(omode & O_CREATE){
    ip = create(r->s1, T_FILE, 0, 0);
    if(ip == 0){
      end_op();
      return -1;
    }
  } else {
    if((ip = namei(r->s1)) == 0){
      end_op();
      return -1;
    }
    ilock(ip);
    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if((h = halloc(ip, 0)) == 0){
    iunlockput(ip);
    end_op();
    return -1;
  }

  if((omode & O_TRUNC) && ip->type == T_FILE){
    vfs_itrunc(ip);
  }

  r->r1 = ip->type;
  r->r2 = ip->major;
  iunlock(ip);
  end_op();
  return h;
}

static long
do_mkdir(char *path)
{
  struct inode *ip;

  begin_op();
  if((ip = create(path, T_DIR, 0, 0)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

static long
do_mknod(char *path, int major, int minor)
{
  struct inode *ip;

  begin_op();
  if((ip = create(path, T_DEVICE, major, minor)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

static long
do_chdir(char *path)
{
  struct inode *ip;
  int h;

  begin_op();
  if((ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  if((h = halloc(ip, 0)) == 0){
    iput(ip);
    end_op();
    return -1;
  }
  end_op();
  return h;
}

static long
do_close(int h)
{
  struct fh *f = hget(h);
  struct inode *ip;

  if(f == 0)
    return -1;
  ip = f->ip;
  f->ip = 0;
  begin_op();
  iput(ip);
  end_op();
  return 0;
}

#ifdef P0_TEST_HOOKS
static int xfer_bad_pid;   // next FS_READ from this client runs the probes
static int xfer_range_pid; // next FS_READ from this client runs one round
static uint64 xfer_range_round;

char *sbrk(int);   // ulib.c; user.h cannot be included together with defs.h

#if defined(__riscv)
// The svc_xfer() prototype takes an int direction, so a 64-bit value would
// be truncated before the syscall.  Call it directly with a wide a1.
static int
svc_xfer_wide(int id, uint64 dir, uint64 caddr, void *buf, int n)
{
  register uint64 a0 asm("a0") = (uint64)id;
  register uint64 a1 asm("a1") = dir;
  register uint64 a2 asm("a2") = caddr;
  register uint64 a3 asm("a3") = (uint64)buf;
  register uint64 a4 asm("a4") = (uint64)n;
  register uint64 a7 asm("a7") = SYS_svc_xfer;

  asm volatile("ecall" : "+r"(a0)
               : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a7)
               : "memory");
  return (int)a0;
}

// Same, with the length widened as well; sys_svc_xfer reads a4 as uint64.
static int
svc_xfer_wide_n(int id, int dir, uint64 caddr, void *buf, uint64 n)
{
  register uint64 a0 asm("a0") = (uint64)id;
  register uint64 a1 asm("a1") = (uint64)dir;
  register uint64 a2 asm("a2") = caddr;
  register uint64 a3 asm("a3") = (uint64)buf;
  register uint64 a4 asm("a4") = n;
  register uint64 a7 asm("a7") = SYS_svc_xfer;

  asm volatile("ecall" : "+r"(a0)
               : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a7)
               : "memory");
  return (int)a0;
}
#endif

// Every invalid direction must be rejected before any copy happens.  Return
// -1 only when all probes are rejected; otherwise a distinct negative value
// lets the client prove which probe was accepted.
static int
bad_xfer_probe(uint64 gaddr)
{
  static char badmark[4] = { 'B', 'A', 'D', '!' };
  long r0, r4, rw = -1;
  int accepted;

  r0 = svc_xfer(SVC_FS, 99, gaddr, badmark, 0);
  r4 = svc_xfer(SVC_FS, 99, gaddr, badmark, 4);
#if defined(__riscv)
  rw = svc_xfer_wide(SVC_FS, 0x100000000ULL, gaddr, badmark, 4);
#endif
  accepted = (r0 != -1) + 2 * (r4 != -1) + 4 * (rw != -1);
  return accepted ? -1 - accepted : -1;
}

static void
range_check(uint64 *bad, int *bit, long got, long want)
{
  if(got != want)
    *bad |= 1ULL << *bit;
  (*bit)++;
}

// One round of boundary probes for sys_svc_xfer().  Returns 0 when every
// probe matched; otherwise a negative value with a deviation bitmask.
static int
xfer_range_probe(struct ipcmsg *r)
{
  static char A[4] = { 'A', 'B', 'C', 'D' };
  static char B[4] = { 'E', 'F', 'G', 'H' };
  static char W[4] = { 'W', 'I', 'D', 'E' };
  static char S9[9] = { 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I' };
  char got[4];
  uint64 bad = 0, g = r->gaddr;
  int bit = 0;

  if(xfer_range_round == 3){
    // The client asked to read into a grant whose end wraps 2^64.
    if(g != 0xFFFFFFFFFFFFFFFCULL || r->glen != 8)
      return -1;
    range_check(&bad, &bit, svc_xfer(SVC_FS, XFER_TO_CLIENT, g, A, 4), -1);
    range_check(&bad, &bit, svc_xfer(SVC_FS, XFER_FROM_CLIENT, g, got, 4), -1);
  } else if(r->glen == 8){
    switch(xfer_range_round){
    case 1:
      range_check(&bad, &bit, svc_xfer(SVC_FS, XFER_TO_CLIENT, g, A, 4), 0);
      range_check(&bad, &bit, svc_xfer(SVC_FS, XFER_TO_CLIENT, g + 4, B, 2), 0);
      range_check(&bad, &bit, svc_xfer(SVC_FS, XFER_FROM_CLIENT, g, got, 4), 0);
      if(memcmp(got, A, 4) != 0)
        bad |= 1ULL << bit;
      bit++;
      range_check(&bad, &bit, svc_xfer(SVC_FS, XFER_TO_CLIENT, g + 6, got, 2), 0);
      break;
    case 2:
      range_check(&bad, &bit, svc_xfer(SVC_FS, XFER_TO_CLIENT, g, S9, 9), -1);
      range_check(&bad, &bit, svc_xfer(SVC_FS, XFER_TO_CLIENT, g + 5, B, 4), -1);
      range_check(&bad, &bit, svc_xfer(SVC_FS, XFER_TO_CLIENT, g + 8, A, 1), -1);
      range_check(&bad, &bit, svc_xfer(SVC_FS, XFER_TO_CLIENT, g + 8, A, 0), 0);
      range_check(&bad, &bit, svc_xfer(SVC_FS, XFER_TO_CLIENT, g - 1, A, 0), -1);
      range_check(&bad, &bit, svc_xfer(SVC_FS, XFER_TO_CLIENT, g - 1, A, 1), -1);
      range_check(&bad, &bit,
                  svc_xfer(SVC_FS, XFER_TO_CLIENT, 0xFFFFFFFFFFFFFFFEULL, A, 4), -1);
      break;
    case 4:
      range_check(&bad, &bit, svc_xfer(SVC_FS, XFER_TO_CLIENT, g, A, -1), -1);
#if defined(__riscv)
      range_check(&bad, &bit,
                  svc_xfer_wide_n(SVC_FS, XFER_TO_CLIENT, g, W, 0x100000004ULL), -1);
      range_check(&bad, &bit,
                  svc_xfer_wide_n(SVC_FS, XFER_TO_CLIENT, g, A,
                                  0xFFFFFFFFFFFFFFFFULL), -1);
#endif
      break;
    case 5:
      range_check(&bad, &bit,
                  svc_xfer(SVC_FS, XFER_TO_CLIENT, g, (void *)0x40000000, 4), -1);
      range_check(&bad, &bit,
                  svc_xfer(SVC_FS, XFER_TO_CLIENT, g, sbrk(0) - 2, 4), -1);
      break;
    default:
      return -1;
    }
  } else {
    return -1;
  }
  return bad ? -1 - (int)bad : 0;
}
#endif

static long
do_read(struct ipcmsg *r, int usepos)
{
  struct fh *f = hget(r->h);
  long n;

  if(f == 0)
    return -1;
#ifdef P0_TEST_HOOKS
  if(r->op == FS_READ && xfer_bad_pid != 0 && r->pid == xfer_bad_pid){
    xfer_bad_pid = 0;
    return bad_xfer_probe(r->gaddr);
  }
  if(r->op == FS_READ && xfer_range_pid != 0 && r->pid == xfer_range_pid){
    int ret;
    xfer_range_pid = 0;
    ret = xfer_range_probe(r);
    xfer_range_round = 0;
    return ret;
  }
#endif
  ilock(f->ip);
  if(usepos){
    n = vfs_readi(f->ip, 1, r->gaddr, (uint)r->a0, (uint)r->a1);
  } else {
    if((n = vfs_readi(f->ip, 1, r->gaddr, f->off, (uint)r->a0)) > 0)
      f->off += n;
  }
  iunlock(f->ip);
  return n;
}

static long
do_write(struct ipcmsg *r)
{
  struct fh *f = hget(r->h);
  int n = r->a0, w, i = 0;

  if(f == 0)
    return -1;
  // write a few blocks at a time to avoid exceeding
  // the maximum log transaction size, including
  // i-node, indirect block, allocation blocks,
  // and 2 blocks of slop for non-aligned writes.
  int max = ((MAXOPBLOCKS-1-1-2) / 2) * BSIZE;
  while(i < n){
    int n1 = n - i;
    if(n1 > max)
      n1 = max;

    begin_op();
    ilock(f->ip);
    if ((w = vfs_writei(f->ip, 1, r->gaddr + i, f->off, n1)) > 0)
      f->off += w;
    iunlock(f->ip);
    end_op();

    if(w != n1){
      // error from writei
      break;
    }
    i += w;
  }
  return i == n ? n : -1;
}

static long
do_stat(struct ipcmsg *r)
{
  struct fh *f = hget(r->h);
  struct stat st;

  if(f == 0)
    return -1;
  ilock(f->ip);
  vfs_stati(f->ip, &st);
  iunlock(f->ip);
  return svc_xfer(SVC_FS, XFER_TO_CLIENT, r->gaddr, &st, sizeof(st));
}

static long
do_dup(int h)
{
  struct fh *f = hget(h);
  int nh;

  if(f == 0)
    return -1;
  if((nh = halloc(f->ip, f->off)) == 0)
    return -1;
  idup(f->ip);
  return nh;
}

static long
dispatch(struct ipcmsg *r)
{
  // Resolve relative paths against the calling process's directory.
  // A nonzero handle this incarnation does not know is an error, not root.
  if(r->cwd != 0){
    struct fh *c = hget(r->cwd);
    if(c == 0)
      return -1;
    fs_cwd = c->ip;
  } else {
    fs_cwd = 0;
  }

  r->s1[MAXPATH-1] = 0;
  r->s2[MAXPATH-1] = 0;
  switch(r->op){
  case FS_OPEN:   return do_open(r);
  case FS_READ:   return do_read(r, 0);
  case FS_WRITE:  return do_write(r);
  case FS_PREAD:  return do_read(r, 1);
  case FS_STAT:   return do_stat(r);
  case FS_CLOSE:  return do_close(r->h);
  case FS_DUP:    return do_dup(r->h);
  case FS_LINK:   return do_link(r->s1, r->s2);
  case FS_UNLINK: return do_unlink(r->s1);
  case FS_MKDIR:  return do_mkdir(r->s1);
  case FS_MKNOD:  return do_mknod(r->s1, r->a0, r->a1);
  case FS_CHDIR:  return do_chdir(r->s1);
  case FS_MOUNT:
#ifdef P0_TEST_HOOKS
    // Arm the direction probes for the next read from this client.
    if(strlen(r->s1) == 11 && strncmp(r->s1, "__xferdir__", 11) == 0){
      xfer_bad_pid = r->pid;
      return 0;
    }
    // Arm one boundary round; the round number travels in mount's dev arg.
    if(strlen(r->s1) == 13 && strncmp(r->s1, "__xferrange__", 13) == 0){
      xfer_range_pid = r->pid;
      xfer_range_round = r->a0;
      return 0;
    }
#endif
#ifdef FSSRV_FAULT_INJECT
    // Test hook (tests/fault): a "mount" of the fake type "__crash__"
    // makes the server dereference a null pointer, as a bug would.
    if(strncmp(r->s1, "__crash__", 10) == 0)
      *(volatile int *)0 = 1;
#endif
    return vfs_mount(r->s1, r->s2, r->a0);
  case FS_UMOUNT: return vfs_umount(r->s1);
  default:        return -1;
  }
}

int
main(void)
{
  long ret;

  // Register first: the kernel only lets the registered server touch the
  // disk.  Clients that call in meanwhile just wait for the first svc_recv.
  if(svc_register(SVC_FS) < 0)
    panic("svc_register");
  binit();
  iinit();
  vfsinit();
  fsinit(ROOTDEV);
  printf("fssrv: file system service up (pid-served)\n");

  for(;;){
    if(svc_recv(SVC_FS, &m) < 0)
      exit(0);
    m.r1 = m.r2 = 0;
    ret = dispatch(&m);
    svc_reply(SVC_FS, ret, m.r1, m.r2);
  }
}
