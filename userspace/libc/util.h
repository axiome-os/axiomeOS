#ifndef AXIOME_LIBC_UTIL_H
#define AXIOME_LIBC_UTIL_H

/* Small shared helpers for the unix-like userspace tools.
   These are static inline so each tool gets its own copy and the linker
   stays dead-simple (no extra TU to add to the per-program libc link). */

#include "errno.h"
#include "syscall.h"
#include <stddef.h>

#ifndef ENOTEMPTY
#define ENOTEMPTY 39
#endif

static inline const char *ax_strerror(int e)
{
    switch (e)
    {
    case 0: return "Success";
    case EPERM: return "Operation not permitted";
    case ENOENT: return "No such file or directory";
    case EIO: return "I/O error";
    case EBADF: return "Bad file descriptor";
    case ENOMEM: return "Out of memory";
    case EACCES: return "Permission denied";
    case EFAULT: return "Bad address";
    case EEXIST: return "File exists";
    case ENOTDIR: return "Not a directory";
    case EISDIR: return "Is a directory";
    case EINVAL: return "Invalid argument";
    case ENOSYS: return "Function not implemented";
    case ENOTEMPTY: return "Directory not empty";
    default: return "Unknown error";
    }
}

/* Format a mode into the classic "-rwxr-xr-x" 10-char string. */
static inline void ax_mode_str(unsigned int m, char out[11])
{
    out[0] = (m & S_IFDIR) ? 'd' : '-';
    out[1] = (m & S_IRUSR) ? 'r' : '-';
    out[2] = (m & S_IWUSR) ? 'w' : '-';
    out[3] = (m & S_IXUSR) ? ((m & S_ISUID) ? 's' : 'x')
                           : ((m & S_ISUID) ? 'S' : '-');
    out[4] = (m & S_IRGRP) ? 'r' : '-';
    out[5] = (m & S_IWGRP) ? 'w' : '-';
    out[6] = (m & S_IXGRP) ? ((m & S_ISGID) ? 's' : 'x')
                           : ((m & S_ISGID) ? 'S' : '-');
    out[7] = (m & S_IROTH) ? 'r' : '-';
    out[8] = (m & S_IWOTH) ? 'w' : '-';
    out[9] = (m & S_IXOTH) ? 'x' : '-';
    out[10] = 0;
}

static inline void ax_join_path(const char *dir, const char *name,
                                char *out, size_t cap)
{
    size_t pl = 0;
    while (dir[pl] && pl + 1 < cap) { out[pl] = dir[pl]; pl++; }
    if (pl > 0 && out[pl - 1] != '/' && pl + 1 < cap) out[pl++] = '/';
    size_t nl = 0;
    while (name[nl] && pl + nl + 1 < cap) { out[pl + nl] = name[nl]; nl++; }
    out[pl + nl] = 0;
}

static inline const char *ax_basename(const char *p)
{
    const char *b = p;
    for (const char *c = p; *c; c++)
        if (*c == '/')
            b = c + 1;
    return b;
}

#endif