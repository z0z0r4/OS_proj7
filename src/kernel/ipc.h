// Microkernel IPC protocol between the kernel and user-space services.
//
// A client system call that needs a service (e.g. open/read/write on a file
// system object) is turned into a synchronous request message.  The kernel
// queues it for the single server process registered for the service and
// puts the client to sleep until the server replies.  Bulk data never lives
// in the message: the server moves it with svc_xfer(), which is only valid
// for the one buffer ("grant") the blocked client handed over with this call.
#ifndef XV6_IPC_H
#define XV6_IPC_H

#include "param.h"

#define SVC_FS     1          // file system service
#define NSVC       2          // number of service ids (index 0 unused)

// Request operations of the file system service.
enum {
  FS_OPEN = 1,   // s1=path a0=omode          -> ret=handle r1=type r2=major
  FS_READ,       // h a0=n    grant=dst       -> ret=bytes (offset kept by server)
  FS_WRITE,      // h a0=n    grant=src       -> ret=bytes
  FS_PREAD,      // h a0=off a1=n grant=dst   -> ret=bytes (kernel exec loader)
  FS_STAT,       // h         grant=struct stat
  FS_CLOSE,      // h
  FS_DUP,        // h                         -> ret=h (extra reference, cwd)
  FS_LINK,       // s1=old s2=new
  FS_UNLINK,     // s1=path
  FS_MKDIR,      // s1=path
  FS_MKNOD,      // s1=path a0=major a1=minor
  FS_CHDIR,      // s1=path                   -> ret=handle of new cwd
  FS_MOUNT,      // s1=fstype s2=path a0=dev
  FS_UMOUNT,     // s1=path
};

// svc_xfer() directions: a closed set.  sys_svc_xfer() rejects any other
// value, and a new direction must be added to its whitelist explicitly.
#define XFER_TO_CLIENT   0
#define XFER_FROM_CLIENT 1

struct ipcmsg {
  int    op;             // FS_*
  int    pid;            // client pid (set by the kernel)
  int    cwd;            // client's cwd handle, 0 = root (set by the kernel)
  int    h;              // object handle
  int    gen;            // handle generation the client believes in (0 = any)
  uint64 a0, a1;
  uint64 gaddr, glen;    // granted client buffer (set by the kernel)
  long   ret, r1, r2;    // reply
  char   s1[MAXPATH];
  char   s2[MAXPATH];
};

#endif
