// Synchronous IPC between clients (any process, via system calls) and the
// user-space service processes, plus the small set of privileged system
// calls a service needs: register, receive, reply, bulk transfer and raw
// block I/O.
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"
#include "ipc.h"

extern struct proc *initproc;

#ifdef P0_TEST_HOOKS
// Test-only: while set, init is held inside svc_restart so that the restart
// authorization check can be observed deterministically while the service is
// down.  Accessed only through the __atomic builtins (cross-CPU shared).
static int svc_hold;

// Test-only: deterministic generation race.  A client arms genrace_pid with
// svc_restart(-3); the next file system ipc_call from that pid consumes it
// once, kills the current server and waits for a new incarnation to
// register, so the request is admitted against the new generation.
// genrace_state is reported through svc_restart(-4): 1 armed, 2 restart
// observed, -1 the hook could not observe a restart.
static int genrace_pid;
static int genrace_state;
#endif

struct svc {
  struct spinlock lock;
  struct proc *server;      // registered server, 0 = service down
  int gen;                  // incarnation; handles of older ones are stale
  int busy;                 // request slot taken by a client
  int has_req;              // request queued, not yet received by server
  int done;                 // reply available
  struct proc *client;
  struct ipcmsg *req;       // lives on the blocked client's kernel stack
  uint64 gaddr, glen;       // grant: the client buffer the server may touch
  int gkernel;              // grant address is a kernel address
  long ret, r1, r2;
} svc[NSVC];

void
ipcinit(void)
{
  int i;
  for(i = 1; i < NSVC; i++)
    initlock(&svc[i].lock, "svc");
}

int
svc_gen(int id)
{
  int g;

  if(id <= 0 || id >= NSVC)
    return 0;
  acquire(&svc[id].lock);
  g = svc[id].gen;
  release(&svc[id].lock);
  return g;
}

int
svc_alive(int id)
{
  return svc[id].server != 0;
}

int
svc_serverpid(int id)
{
  struct proc *s = svc[id].server;
  return s ? s->pid : 0;
}

#ifdef P0_TEST_HOOKS
// Run by the armed client at the top of ipc_call, before the request is
// admitted: kill the current server and wait, without holding svc.lock or
// tickslock across kkill/yield, until a new incarnation has registered.
// The caller's request is then admitted against that new generation.
static void
ipc_genrace_hook(int id, struct proc *p)
{
  struct svc *s;
  int expected, old, cur;
  uint start, now;

  if(id != SVC_FS)
    return;
  expected = p->pid;
  if(!__atomic_compare_exchange_n(&genrace_pid, &expected, 0, 0,
                                  __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return;
  s = &svc[id];
  acquire(&s->lock);
  old = s->server ? s->server->pid : 0;
  release(&s->lock);
  if(old <= 0){
    __atomic_store_n(&genrace_state, -1, __ATOMIC_RELEASE);
    return;
  }
  kkill(old);
  acquire(&tickslock);
  start = ticks;
  release(&tickslock);
  cur = 0;
  for(;;){
    acquire(&s->lock);
    cur = s->server ? s->server->pid : 0;
    release(&s->lock);
    if(cur > 0 && cur != old)
      break;
    if(killed(p))
      break;
    acquire(&tickslock);
    now = ticks;
    release(&tickslock);
    if(now - start >= 100)      // bounded; normal restart is much faster
      break;
    yield();
  }
  __atomic_store_n(&genrace_state, (cur > 0 && cur != old) ? 2 : -1,
                   __ATOMIC_RELEASE);
}
#endif

// Send m to service id and wait for the reply.  The client's buffer
// [gaddr, gaddr+glen) (user address, or kernel address if gkernel) is
// made accessible to the server for the duration of the call.
// If callgen is set, it receives the generation of the server incarnation
// that admitted the request (0 if it was never admitted).  Returns m->ret,
// or -1 if the service is down, died, or the handle in m->gen belongs to a
// previous incarnation.
long
ipc_call(int id, struct ipcmsg *m, uint64 gaddr, uint64 glen, int gkernel,
         int *callgen)
{
  struct svc *s = &svc[id];
  struct proc *p = myproc();
  long ret;
  int serving = 0;

  if(callgen)
    *callgen = 0;
#ifdef P0_TEST_HOOKS
  ipc_genrace_hook(id, p);
#endif
  m->pid = p->pid;
  m->gaddr = gaddr;
  m->glen = glen;
  acquire(&s->lock);
  for(;;){
    if(s->server == 0 || (m->gen != 0 && m->gen != s->gen)){
      release(&s->lock);
      return -1;
    }
    if(!s->busy)
      break;
    sleep(&s->busy, &s->lock);
  }
  // Admitted: the server pointer and generation were just checked under the
  // lock, so capture the serving incarnation in the same critical section.
  serving = s->gen;
  s->busy = 1;
  s->client = p;
  s->req = m;
  s->gaddr = gaddr;
  s->glen = glen;
  s->gkernel = gkernel;
  s->done = 0;
  s->has_req = 1;
  wakeup(&s->has_req);
  // Uninterruptible: the server may be writing into our memory.
  while(!s->done)
    sleep(p, &s->lock);
  ret = s->ret;
  m->r1 = s->r1;
  m->r2 = s->r2;
  s->busy = 0;
  s->client = 0;
  s->req = 0;
  wakeup(&s->busy);
  release(&s->lock);
  if(callgen)
    *callgen = serving;
  return ret;
}

// Called when process p exits: if it was a server, fail the request in
// flight and everybody queued behind it; clients see -1 until a new
// server registers.
void
ipc_proc_exit(struct proc *p)
{
  int i;
  for(i = 1; i < NSVC; i++){
    struct svc *s = &svc[i];
    acquire(&s->lock);
    if(s->server == p){
      s->server = 0;
      if(s->busy && !s->done){
        s->ret = -1;
        s->r1 = s->r2 = 0;
        s->done = 1;
        s->has_req = 0;
        wakeup(s->client);
      }
      wakeup(&s->busy);
    }
    release(&s->lock);
  }
#ifdef P0_TEST_HOOKS
  {
    int expected = p->pid;
    if(__atomic_compare_exchange_n(&genrace_pid, &expected, 0, 0,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
      __atomic_store_n(&genrace_state, -1, __ATOMIC_RELEASE);
  }
#endif
  p->xfer_client = 0;
}

// svc_register(id): become the server of service id.
uint64
sys_svc_register(void)
{
  int id;
  struct svc *s;

  argint(0, &id);
  if(id <= 0 || id >= NSVC)
    return -1;
  s = &svc[id];
  // only the process the kernel started from the boot module may serve
  if(myproc()->svcid != id)
    return -1;
  acquire(&s->lock);
  if(s->server != 0){
    release(&s->lock);
    return -1;
  }
  s->server = myproc();
  if(s->gen == 0x7fffffff)
    s->gen = 1;
  else
    s->gen++;
  release(&s->lock);
  return s->gen;
}

// svc_recv(id, struct ipcmsg *): wait for the next request.
uint64
sys_svc_recv(void)
{
  int id;
  uint64 uaddr;
  struct svc *s;
  struct proc *p = myproc();
  struct ipcmsg m;

  argint(0, &id);
  argaddr(1, &uaddr);
  if(id <= 0 || id >= NSVC)
    return -1;
  s = &svc[id];
  acquire(&s->lock);
  if(s->server != p){
    release(&s->lock);
    return -1;
  }
  while(!s->has_req){
    if(killed(p)){
      release(&s->lock);
      return -1;
    }
    sleep(&s->has_req, &s->lock);
  }
  m = *s->req;
  s->has_req = 0;
  p->xfer_client = s->client;
  release(&s->lock);
  if(copyout(p->pagetable, uaddr, (char *)&m, sizeof(m)) < 0)
    return -1;
  return 0;
}

// svc_reply(id, ret, r1, r2)
uint64
sys_svc_reply(void)
{
  int id;
  uint64 ret, r1, r2;
  struct svc *s;
  struct proc *p = myproc();

  argint(0, &id);
  argaddr(1, &ret);
  argaddr(2, &r1);
  argaddr(3, &r2);
  if(id <= 0 || id >= NSVC)
    return -1;
  s = &svc[id];
  acquire(&s->lock);
  if(s->server != p || !s->busy || s->done || s->has_req){
    release(&s->lock);
    return -1;
  }
  s->ret = (long)ret;
  s->r1 = (long)r1;
  s->r2 = (long)r2;
  s->glen = 0;
  s->done = 1;
  p->xfer_client = 0;
  wakeup(s->client);
  release(&s->lock);
  return 0;
}

// svc_xfer(id, dir, caddr, buf, n): copy between the server's buf and
// the granted client buffer at caddr.
uint64
sys_svc_xfer(void)
{
  int id, n;
  uint64 dir, caddr, buf, n64;
  struct svc *s;
  struct proc *p = myproc(), *c;
  char tmp[256];
  int gkernel;
  uint64 gaddr, glen, off;

  argint(0, &id);
  argaddr(1, &dir);
  argaddr(2, &caddr);
  argaddr(3, &buf);
  argaddr(4, &n64);
  // Directions are a closed set: anything else must not touch any buffer.
  if(dir != XFER_TO_CLIENT && dir != XFER_FROM_CLIENT)
    return -1;
  // Length is a 32-bit signed protocol field.  Reject any value that
  // would not survive the cast, including sign-extended negatives.
  if(n64 > 0x7fffffffULL)
    return -1;
  n = (int)n64;
  if(id <= 0 || id >= NSVC)
    return -1;
  s = &svc[id];
  acquire(&s->lock);
  if(s->server != p || !s->busy || s->done || s->has_req || s->glen == 0){
    release(&s->lock);
    return -1;
  }
  c = s->client;
  gaddr = s->gaddr;
  glen = s->glen;
  gkernel = s->gkernel;
  release(&s->lock);

  // A wrapping grant has no representable end; reject it before any
  // subtraction-based containment test.
  if(glen > ~0ULL - gaddr)
    return -1;
  // [caddr, caddr+n) must lie inside [gaddr, gaddr+glen).
  if(caddr < gaddr)
    return -1;
  off = caddr - gaddr;
  if(off > glen || (uint64)n > glen - off)
    return -1;
  while(n > 0){
    int m = n > (int)sizeof(tmp) ? (int)sizeof(tmp) : n;
    if(dir == XFER_FROM_CLIENT){
      if(gkernel)
        memmove(tmp, (void *)caddr, m);
      else if(copyin(c->pagetable, tmp, caddr, m) < 0)
        return -1;
      if(copyout(p->pagetable, buf, tmp, m) < 0)
        return -1;
    } else {
      if(copyin(p->pagetable, tmp, buf, m) < 0)
        return -1;
      if(gkernel)
        memmove((void *)caddr, tmp, m);
      else if(copyout(c->pagetable, caddr, tmp, m) < 0)
        return -1;
    }
    caddr += m;
    buf += m;
    n -= m;
  }
  return 0;
}

// blk_rw(dev, blockno, buf, write): raw block I/O for the file system
// server; the only disk access path left in the kernel (driver + HAL).
uint64
sys_blk_rw(void)
{
  int dev, blockno, write;
  uint64 uaddr;
  struct buf *b;
  struct proc *p = myproc();

  argint(0, &dev);
  argint(1, &blockno);
  argaddr(2, &uaddr);
  argint(3, &write);
  if(svc[SVC_FS].server != p)
    return -1;
  if((b = kalloc()) == 0)
    return -1;
  memset(b, 0, sizeof(*b));
  initsleeplock(&b->lock, "blk");
  b->dev = dev;
  b->blockno = blockno;
  if(write){
    if(copyin(p->pagetable, (char *)b->data, uaddr, BSIZE) < 0){
      kfree(b);
      return -1;
    }
    hal_disk_rw(b, 1);
  } else {
    hal_disk_rw(b, 0);
    if(copyout(p->pagetable, uaddr, (char *)b->data, BSIZE) < 0){
      kfree(b);
      return -1;
    }
  }
  kfree(b);
  return 0;
}

// svc_pid(id): pid of the live server, 0 if the service is down.
uint64
sys_svc_pid(void)
{
  int id;
  argint(0, &id);
  if(id <= 0 || id >= NSVC)
    return -1;
  return svc_serverpid(id);
}

// svc_restart(id): re-spawn a dead service from its boot module.
uint64
sys_svc_restart(void)
{
  int id;
  struct proc *p = myproc();

  argint(0, &id);
#ifdef P0_TEST_HOOKS
  if(id == -2){
    __atomic_store_n(&svc_hold, 1, __ATOMIC_RELEASE);
    return 0;
  }
  if(id == -1){
    __atomic_store_n(&svc_hold, 0, __ATOMIC_RELEASE);
    return 0;
  }
  if(id == -3){
    // Arm a one-shot generation race: this process's next file system IPC
    // restarts the server before the request is admitted.
    __atomic_store_n(&genrace_pid, p->pid, __ATOMIC_RELEASE);
    __atomic_store_n(&genrace_state, 1, __ATOMIC_RELEASE);
    return 0;
  }
  if(id == -4)
    return __atomic_load_n(&genrace_state, __ATOMIC_ACQUIRE);
  if(p == initproc && __atomic_load_n(&svc_hold, __ATOMIC_ACQUIRE)){
    uint start, now;
    acquire(&tickslock);
    start = ticks;
    release(&tickslock);
    while(__atomic_load_n(&svc_hold, __ATOMIC_ACQUIRE)){
      acquire(&tickslock);
      now = ticks;
      release(&tickslock);
      if(now - start >= 300)   // bounded; the test releases the hold much earlier
        break;
      yield();
    }
  }
#endif
  // Only init supervises the file system service.  Without this, any process
  // could spawn a second server while the service is down, and the loser of
  // the registration race panics.
  if(p != initproc)
    return -1;
  if(id != SVC_FS)
    return -1;
  if(svc[SVC_FS].server != 0)
    return -1;
  return spawn_fsserver();
}

// ---- file system client helpers (used by the system call layer) ----

enum { FS_CWD_NONE, FS_CWD_ABS, FS_CWD_REL, FS_CWD_INVALID };

// How does this request use the caller's current directory?
static int
fs_cwd_need(struct ipcmsg *m)
{
  switch(m->op){
  case FS_READ:
  case FS_WRITE:
  case FS_PREAD:
  case FS_STAT:
  case FS_CLOSE:
  case FS_DUP:
    return FS_CWD_NONE;
  case FS_OPEN:
  case FS_UNLINK:
  case FS_MKDIR:
  case FS_MKNOD:
  case FS_CHDIR:
  case FS_UMOUNT:
    return m->s1[0] == '/' ? FS_CWD_ABS : FS_CWD_REL;
  case FS_LINK:
    return (m->s1[0] == '/' && m->s2[0] == '/') ? FS_CWD_ABS : FS_CWD_REL;
  case FS_MOUNT:
    return m->s2[0] == '/' ? FS_CWD_ABS : FS_CWD_REL;
  default:
    return FS_CWD_INVALID;      // unknown op: never guess a cwd policy
  }
}

// Root is exactly (h=0, gen=0).  A usable remote cwd is exactly
// (h>0, gen>0).  Anything else is a lost or inconsistent handle: relative
// names must fail instead of silently resolving from the root.
static int
fs_cwd_usable(struct proc *p)
{
  if(p->cwdstale)
    return 0;
  if(p->cwdh == 0)
    return p->cwdgen == 0;
  return p->cwdh > 0 && p->cwdgen > 0;
}

// Call the file system server on behalf of the current process.
long
fs_call(struct ipcmsg *m, uint64 gaddr, uint64 glen, int gkernel, int *callgen)
{
  struct proc *p = myproc();
  int need = fs_cwd_need(m);

  if(callgen)
    *callgen = 0;
  if(need == FS_CWD_NONE){
    m->cwd = 0;                 // do not touch m->gen: it is the handle generation
  } else if(need == FS_CWD_INVALID){
    return -1;                  // fail closed before any IPC
  } else if(need == FS_CWD_ABS){
    m->cwd = 0;                 // absolute paths are cwd-independent
    m->gen = 0;
  } else {
    if(!fs_cwd_usable(p))
      return -1;                // explicit failure, never fall back to root
    m->cwd = p->cwdh;           // 0 = root, which is incarnation-independent
    m->gen = p->cwdgen;         // ipc_call rejects under s->lock if stale
  }
  return ipc_call(SVC_FS, m, gaddr, glen, gkernel, callgen);
}

// Drop a server handle; stale handles (dead server) need no message.
void
fs_release(int h, int gen)
{
  struct ipcmsg m;

  if(h <= 0 || gen != svc_gen(SVC_FS) || !svc_alive(SVC_FS))
    return;
  memset(&m, 0, sizeof(m));
  m.op = FS_CLOSE;
  m.h = h;
  m.gen = gen;
  ipc_call(SVC_FS, &m, 0, 0, 0, 0);
}

// Give a new process its own reference to p's cwd.
void
fs_dup_cwd(struct proc *np, struct proc *p)
{
  struct ipcmsg m;

  np->cwdh = 0;
  np->cwdgen = 0;
  np->cwdstale = 0;

  if(p->cwdstale){
    np->cwdstale = 1;           // parent already lost its cwd
    return;
  }
  if(p->cwdh == 0 && p->cwdgen == 0)
    return;                     // valid root: nothing to copy
  if(!fs_cwd_usable(p)){
    np->cwdstale = 1;           // inconsistent/lost handle: never root
    return;
  }

  memset(&m, 0, sizeof(m));
  m.op = FS_DUP;
  m.h = p->cwdh;
  m.gen = p->cwdgen;            // >0, so ipc_call compares generations under the lock
  long h = ipc_call(SVC_FS, &m, 0, 0, 0, 0);
  if(h > 0){
    np->cwdh = h;
    np->cwdgen = p->cwdgen;
  } else {
    np->cwdstale = 1;           // stale generation or failed dup: not root
  }
}
