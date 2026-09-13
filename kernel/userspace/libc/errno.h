#ifndef AXIOME_LIBC_ERRNO_H
#define AXIOME_LIBC_ERRNO_H

extern int errno;

#define ENOSYS 38
#define EINVAL 22
#define ENOMEM 12
#define ENOENT 2
#define EPERM  1
#define ESRCH  3
#define EFAULT 14
#define EACCES 13
#define EEXIST 17
#define ENOTDIR 20
#define EISDIR 21
#define EBADF  9
#define ECHILD 10
#define EAGAIN 11
#define EIO    5

/* perror - print error message to stderr */
void perror(const char *s);

#endif
