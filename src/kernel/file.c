//
// Support functions for system calls that involve file descriptors.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "fs.h"
#include "spinlock.h"
#include "file.h"
#include "stat.h"
#include "proc.h"
#include "ipc.h"

struct devsw devsw[NDEV];
struct {
  struct spinlock lock;
  struct file file[NFILE];
} ftable;

void
fileinit(void)
{
  initlock(&ftable.lock, "ftable");
}

// Allocate a file structure.
struct file*
filealloc(void)
{
  struct file *f;

  acquire(&ftable.lock);
  for(f = ftable.file; f < ftable.file + NFILE; f++){
    if(f->ref == 0){
      f->ref = 1;
      release(&ftable.lock);
      return f;
    }
  }
  release(&ftable.lock);
  return 0;
}

// Increment ref count for file f.
struct file*
filedup(struct file *f)
{
  acquire(&ftable.lock);
  if(f->ref < 1)
    panic("filedup");
  f->ref++;
  release(&ftable.lock);
  return f;
}

// Close file f.  (Decrement ref count, close when reaches 0.)
void
fileclose(struct file *f)
{
  struct file ff;

  acquire(&ftable.lock);
  if(f->ref < 1)
    panic("fileclose");
  if(--f->ref > 0){
    release(&ftable.lock);
    return;
  }
  ff = *f;
  f->ref = 0;
  f->type = FD_NONE;
  release(&ftable.lock);

  if(ff.type == FD_PIPE){
    pipeclose(ff.pipe, ff.writable);
  } else if((ff.type == FD_REMOTE || ff.type == FD_DEVICE) && ff.h > 0){
    fs_release(ff.h, ff.gen);
  }
}

// Get metadata about file f.
// addr is a user virtual address, pointing to a struct stat.
int
filestat(struct file *f, uint64 addr)
{
  struct ipcmsg m;
  struct stat st;

  if(f->type == FD_DEVICE && f->h == 0){
    // console opened by the kernel for the file system server itself
    memset(&st, 0, sizeof(st));
    st.type = T_DEVICE;
    st.nlink = 1;
    return copyout(myproc()->pagetable, addr, (char *)&st, sizeof(st));
  }
  if(f->type == FD_REMOTE || f->type == FD_DEVICE){
    memset(&m, 0, sizeof(m));
    m.op = FS_STAT;
    m.h = f->h;
    m.gen = f->gen;
    return fs_call(&m, addr, sizeof(st), 0, 0);
  }
  return -1;
}

// Read from file f.
// addr is a user virtual address.
int
fileread(struct file *f, uint64 addr, int n)
{
  int r = 0;

  if(f->readable == 0)
    return -1;

  if(f->type == FD_PIPE){
    r = piperead(f->pipe, addr, n);
  } else if(f->type == FD_DEVICE){
    if(f->major < 0 || f->major >= NDEV || !devsw[f->major].read)
      return -1;
    r = devsw[f->major].read(1, addr, n);
  } else if(f->type == FD_REMOTE){
    struct ipcmsg m;
    memset(&m, 0, sizeof(m));
    m.op = FS_READ;
    m.h = f->h;
    m.gen = f->gen;
    m.a0 = (uint)n;
    r = fs_call(&m, addr, (uint)n, 0, 0);
  } else {
    panic("fileread");
  }

  return r;
}

// Write to file f.
// addr is a user virtual address.
int
filewrite(struct file *f, uint64 addr, int n)
{
  int ret = 0;

  if(f->writable == 0)
    return -1;

  if(f->type == FD_PIPE){
    ret = pipewrite(f->pipe, addr, n);
  } else if(f->type == FD_DEVICE){
    if(f->major < 0 || f->major >= NDEV || !devsw[f->major].write)
      return -1;
    ret = devsw[f->major].write(1, addr, n);
  } else if(f->type == FD_REMOTE){
    struct ipcmsg m;
    memset(&m, 0, sizeof(m));
    m.op = FS_WRITE;
    m.h = f->h;
    m.gen = f->gen;
    m.a0 = (uint)n;
    ret = fs_call(&m, addr, (uint)n, 0, 0);
  } else {
    panic("filewrite");
  }

  return ret;
}

