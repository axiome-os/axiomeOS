#ifndef AXIOME_SECURITY_H
#define AXIOME_SECURITY_H

#include <stdint.h>
#include <stddef.h>

/* Basic scalar types must be visible before vfs.h (whose struct vnode
   references them). The rest of the security types are defined below,
   after vfs.h has been pulled in. */
typedef uint32_t uid_t;
typedef uint32_t gid_t;

enum user_role {
    ROLE_GUEST  = 0,
    ROLE_USER   = 1,
    ROLE_ADMIN  = 2,
    ROLE_SYSTEM = 3,
    NROLES
};

typedef enum user_role user_role_t;

#include "vfs.h"

/* ---------------------------------------------------------------- *
 * Security model (see docs/user-rank-system-spec.md)
 *
 * Capabilities are a fine-grained bitmask. A process's effective
 * capabilities are derived from its role. The POSIX uid/gid layer
 * is a compatibility shim on top: a uid maps to a role, and the
 * role maps to a bounded set of capabilities. No process (not even
 * POSIX root, uid 0) escapes capability checks -- only the kernel
 * SYSTEM role (used by init and OS daemons) is exempt, by design.
 * ---------------------------------------------------------------- */

struct thread;   /* forward decl; full definition comes from sched.h */

/* ---- capability flags ---- */
#define CAP_FORK            (1ULL <<  0)
#define CAP_FILE_READ       (1ULL <<  1)
#define CAP_FILE_WRITE_SELF (1ULL <<  2)
#define CAP_FILE_WRITE_ANY  (1ULL <<  3)
#define CAP_KILL_SELF       (1ULL <<  4)
#define CAP_KILL_ANY        (1ULL <<  5)
#define CAP_MOUNT           (1ULL <<  6)
#define CAP_UMOUNT          (1ULL <<  7)
#define CAP_DRIVER          (1ULL <<  8)
#define CAP_NET             (1ULL <<  9)
#define CAP_NET_RAW         (1ULL << 10)
#define CAP_IPC             (1ULL << 11)
#define CAP_SETUID          (1ULL << 12)
#define CAP_SETGID          (1ULL << 13)
#define CAP_USER_MGMT       (1ULL << 14)
#define CAP_SERVICE_MGMT    (1ULL << 15)
#define CAP_SYS_ADMIN       (1ULL << 16)
#define CAP_SIGNAL_OTHER    (1ULL << 17)
#define CAP_PTRACE          (1ULL << 18)
#define CAP_BOOT            (1ULL << 19)
#define CAP_TIME            (1ULL << 20)
#define CAP_RESOURCE        (1ULL << 21)
#define CAP_EXECUTE         (1ULL << 22)
#define CAP_FILE_WRITE_TMP  (1ULL << 23)

/* ---- roles ---- */
/* (enum user_role / user_role_t are defined at the top of this file, before
   vfs.h, because struct vnode references them.) */

/* role -> capability set (static table, filled in security.c) */
extern const uint64_t role_caps[NROLES];

/* ---- VFS permission bits (POSIX subset) ---- */
#define S_ISUID  04000
#define S_ISGID  02000
#define S_IRUSR  00400
#define S_IWUSR  00200
#define S_IXUSR  00100
#define S_IRGRP  00040
#define S_IWGRP  00020
#define S_IXGRP  00010
#define S_IROTH  00004
#define S_IWOTH  00002
#define S_IXOTH  00001

/* ---- errno values used by security/VFS paths ---- */
#define EPERM    1
#define ENOENT   2
#define ESRCH    3
#define EINTR    4
#define EIO      5
#define EBADF    9
#define ECHILD   10
#define EAGAIN   11
#define ENOMEM   12
#define EACCES   13
#define EFAULT   14
#define EEXIST   17
#define ENOTDIR  20
#define EISDIR   21
#define EINVAL   22
#define ENOTEMPTY 39
#define ESPIPE   29
#define ENOSYS   38

/* ---- user database (parsed from /etc/passwd) ---- */
#define MAX_USERS 64

struct user_entry {
    char name[32];
    uid_t uid;
    gid_t gid;
    user_role_t role;
    char passwd_hash[65];
    char home[128];
    char shell[64];
};

extern struct user_entry g_users[MAX_USERS];
extern int g_nusers;

/* ---- runtime security helpers ---- */
int sec_check_cap(struct thread *t, uint64_t cap);
void security_init(void);
void security_reload(void);
int security_authenticate(const char *username, const char *password);
const struct user_entry *security_lookup_name(const char *name);
const struct user_entry *security_lookup_uid(uid_t uid);

/* ---- POSIX adapter (kernel/posix.c) ---- */
user_role_t posix_uid_to_role(uid_t uid);
uid_t posix_role_next_uid(user_role_t role);

#endif
