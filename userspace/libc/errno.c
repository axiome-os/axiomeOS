#include "errno.h"
#include "stdio.h"
#include "syscall.h"
#include "string.h"

int errno = 0;

static const char *strerror_internal(int errnum)
{
    switch (errnum)
    {
    case 0: return "Success";
    case EPERM: return "Operation not permitted";
    case ENOENT: return "No such file or directory";
    case ESRCH: return "No such process";
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
    default: return "Unknown error";
    }
}

void perror(const char *s)
{
    const char *errstr = strerror_internal(errno);
    if (s && *s)
    {
        write(2, s, strlen(s));
        write(2, ": ", 2);
    }
    write(2, errstr, strlen(errstr));
    write(2, "\n", 1);
}
