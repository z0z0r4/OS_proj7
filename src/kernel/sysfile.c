//
// File-system system calls.  The file system itself is a user-space
// server (see fssrv/); these are argument checking and IPC stubs.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "file.h"
#include "fcntl.h"
#include "ipc.h"

// Send a request with up to two path arguments to the file system server.
static long
pathcall(int op, char *s1, char *s2, uint64 a0, uint64 a1, int *callgen)
{
  struct ipcmsg m;

  memset(&m, 0, sizeof(m));
  m.op = op;
  if(s1)
    safestrcpy(m.s1, s1, sizeof(m.s1));
  if(s2)
    safestrcpy(m.s2, s2, sizeof(m.s2));
  m.a0 = a0;
  m.a1 = a1;
  return fs_call(&m, 0, 0, 0, callgen);
}

uint64
sys_mount(void)
{
  char fstype[16], path[MAXPATH];
  int dev;

  if(argstr(0, fstype, sizeof(fstype)) < 0 ||
     argstr(1, path, MAXPATH) < 0)
    return -1;
  argint(2, &dev);

  return pathcall(FS_MOUNT, fstype, path, dev, 0, 0);
}

uint64
sys_umount(void)
{
  char path[MAXPATH];

  if(argstr(0, path, MAXPATH) < 0)
    return -1;
  return pathcall(FS_UMOUNT, path, 0, 0, 0, 0);
}

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  argint(n, &fd);
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *p = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd] == 0){
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

uint64
sys_dup(void)
{
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

uint64
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  argaddr(1, &p);
  argint(2, &n);
  if(argfd(0, 0, &f) < 0)
    return -1;
  return fileread(f, p, n);
}

uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;
  
  argaddr(1, &p);
  argint(2, &n);
  if(argfd(0, 0, &f) < 0)
    return -1;

  return filewrite(f, p, n);
}

uint64
sys_close(void)
{
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

uint64
sys_fstat(void)
{
  struct file *f;
  uint64 st; // user pointer to struct stat

  argaddr(1, &st);
  if(argfd(0, 0, &f) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
uint64
sys_link(void)
{
  char new[MAXPATH], old[MAXPATH];

  if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;
  return pathcall(FS_LINK, old, new, 0, 0, 0);
}

uint64
sys_unlink(void)
{
  char path[MAXPATH];

  if(argstr(0, path, MAXPATH) < 0)
    return -1;
  return pathcall(FS_UNLINK, path, 0, 0, 0, 0);
}

uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode, gen = 0;
  struct file *f;
  struct ipcmsg m;
  long h;

  argint(1, &omode);
  if(argstr(0, path, MAXPATH) < 0)
    return -1;

  memset(&m, 0, sizeof(m));
  m.op = FS_OPEN;
  safestrcpy(m.s1, path, sizeof(m.s1));
  m.a0 = omode;
  if((h = fs_call(&m, 0, 0, 0, &gen)) <= 0)
    return -1;

  if(m.r1 == T_DEVICE && (m.r2 < 0 || m.r2 >= NDEV)){
    fs_release(h, gen);
    return -1;
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    fs_release(h, gen);
    return -1;
  }

  if(m.r1 == T_DEVICE){
    f->type = FD_DEVICE;
    f->major = m.r2;
  } else {
    f->type = FD_REMOTE;
  }
  f->h = h;
  f->gen = gen;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);
  return fd;
}

uint64
sys_mkdir(void)
{
  char path[MAXPATH];

  if(argstr(0, path, MAXPATH) < 0)
    return -1;
  return pathcall(FS_MKDIR, path, 0, 0, 0, 0);
}

uint64
sys_mknod(void)
{
  char path[MAXPATH];
  int major, minor;

  argint(1, &major);
  argint(2, &minor);
  if(argstr(0, path, MAXPATH) < 0)
    return -1;
  return pathcall(FS_MKNOD, path, 0, major, minor, 0);
}

uint64
sys_chdir(void)
{
  char path[MAXPATH];
  struct proc *p = myproc();
  int gen = 0;
  long h;

  if(argstr(0, path, MAXPATH) < 0)
    return -1;
  if((h = pathcall(FS_CHDIR, path, 0, 0, 0, &gen)) <= 0)
    return -1;
  fs_release(p->cwdh, p->cwdgen);
  p->cwdh = h;
  p->cwdgen = gen;
  p->cwdstale = 0;
  return 0;
}

uint64
sys_exec(void)
{
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  argaddr(1, &uargv);
  if(argstr(0, path, MAXPATH) < 0) {
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv)){
      goto bad;
    }
    if(fetchaddr(uargv+sizeof(uint64)*i, (uint64*)&uarg) < 0){
      goto bad;
    }
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if(argv[i] == 0)
      goto bad;
    if(fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }

  int ret = kexec(path, argv);

  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  return ret;

 bad:
  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
}

uint64
sys_pipe(void)
{
  uint64 fdarray; // user pointer to array of two integers
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  argaddr(0, &fdarray);
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      p->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  if(copyout(p->pagetable, fdarray, (char*)&fd0, sizeof(fd0)) < 0 ||
     copyout(p->pagetable, fdarray+sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0){
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}
