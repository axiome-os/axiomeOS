#include "syscall.h"
#include "clock.h"
#include "printk.h"
#include "sched.h"
#include "tss.h"
#include "elf.h"
#include "vmm.h"
#include "serial.h"
#include "framebuffer.h"
#include "keyboard.h"
#include "tty.h"
#include "driver.h"
#include "slab.h"
#include "string.h"
#include "pmm.h"
#include "vfs.h"
#include "fat32.h"
#include "axiomefs.h"
#include "socket.h"
#include "dns.h"
#include "icmp.h"
#include "security.h"
#include "module.h"
#include "signal.h"
#include "idt.h"

/* SEEK_* whence constants (must match userspace libc/syscall.h). */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

uint64_t syscall_user_rsp;
uint64_t current_kstack_top;
void syscall_set_frame(uint64_t *f)
{
    struct thread *t = sched_current();
    if (t)
        t->syscall_iret = f;
}
struct { uint64_t rip, rsp, rflags, rbx, rbp, r12, r13, r14, r15; } syscall_uregs;
static uint8_t syscall_stack_page[262144] __attribute__((aligned(16)));

/* ---- pipe / FIFO implementation (in-kernel buffered channel) ---- */

static int fd_alloc(struct thread *t)
{
    for (int i = 0; i < MAX_FD; i++)
        if (!t->fds[i].used) return i;
    return -1;
}

static uint64_t sys_write_pipe(struct vfs_file *f, const char *s, size_t len);

extern void syscall_entry(void);

typedef uint64_t (*syscall_fn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

/* ---- validated userspace access (copy-in / copy-out) ----
   Every user pointer crossing the syscall boundary must go through these.
   Ranges are checked against the current process's page table (present +
   MMU_USER, plus MMU_WRITE for write access) before any byte is touched, so a
   NULL / wild / unmapped pointer returns -EFAULT instead of faulting or being
   silently demand-mapped by the page-fault handler. */

static int user_range_ok(uint64_t uptr, size_t n, int write)
{
    if (n == 0)
        return 1;
    if (uptr < USERSPACE_BASE)
        return 0;
    if (uptr > UINT64_MAX - n)
        return 0;
    struct thread *t = sched_current();
    struct mmu_root *root = t ? t->mmu : 0;
    if (!root)
        root = vmm_kernel_root();
    return vmm_check_user_range(root, uptr, n, write);
}

static long copy_to_user(void *dst, const void *src, size_t n)
{
    if (n == 0)
        return 0;
    if (!user_range_ok((uint64_t)dst, n, 1))
        return -EFAULT;
    memcpy(dst, src, n);
    return 0;
}

static long copy_from_user(void *dst, const void *src, size_t n)
{
    if (n == 0)
        return 0;
    if (!user_range_ok((uint64_t)src, n, 0))
        return -EFAULT;
    memcpy(dst, src, n);
    return 0;
}

/* Copy a NUL-terminated string from user space into a kernel buffer of `max`
   bytes. Returns the number of bytes copied (including NUL) or -EFAULT. */
static long copy_user_str(uint64_t uptr, char *dst, size_t max)
{
    if (!uptr)
        return -EFAULT;
    if (max == 0)
        return 0;
    size_t copied = 0;
    while (copied < max - 1)
    {
        uint64_t va = uptr + copied;
        uint64_t page_left = PAGE_SIZE - (va & (PAGE_SIZE - 1));
        size_t chunk = page_left;
        size_t remain = max - 1 - copied;
        if (chunk > remain)
            chunk = remain;
        if (!user_range_ok(va, chunk, 0))
            return -EFAULT;
        for (size_t i = 0; i < chunk; i++)
        {
            char c = ((const char *)(uintptr_t)va)[i];
            dst[copied] = c;
            copied++;
            if (c == 0)
                goto done;
        }
    }
done:
    dst[copied] = 0;
    return (long)copied;
}

static uint64_t sys_print(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    char buf[256];
    if (copy_user_str(a1, buf, sizeof(buf)) < 0)
        return (uint64_t)-EFAULT;
    printk("SYS_print: %s\n", buf);
    (void)a2; (void)a3; (void)a4; (void)a5;
    return 0;
}

static uint64_t sys_yield(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    sched_yield();
    return 0;
}

static uint64_t sys_exit(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a2; (void)a3; (void)a4; (void)a5;
    sched_exit((int)a1);
    return 0;
}

static uint64_t sys_fork(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    return (uint64_t)fork_process(syscall_uregs.rip,
                                  syscall_uregs.rsp,
                                  syscall_uregs.rflags,
                                  syscall_uregs.rbx,
                                  syscall_uregs.rbp,
                                  syscall_uregs.r12,
                                  syscall_uregs.r13,
                                  syscall_uregs.r14,
                                  syscall_uregs.r15);
}

static uint64_t sys_getpid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    struct thread *t = sched_current();
    return (uint64_t)(t ? t->pid : 0);
}

static uint64_t sys_waitpid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a3; (void)a4; (void)a5;
    int pid_filter = (int)a1;
    int *status_out = (int *)a2;
    struct thread *t = sched_current();
    int self_pid = t ? t->pid : 0;

    if (status_out && !user_range_ok((uint64_t)status_out, sizeof(int), 1))
        return (uint64_t)-EFAULT;

    for (;;)
    {
        struct thread *z = sched_child_zombie(self_pid, pid_filter);
        if (z)
        {
            int st = z->exit_status;
            int zpid = z->pid;
            sched_reap_zombie(z);
            if (status_out)
                *status_out = st;
            return (uint64_t)zpid;
        }
        if (!sched_has_child(self_pid))
            return (uint64_t)(-1);
        sched_suspend();
    }
}

/* Non-blocking reap for event loops (the axwm compositor polls this every
   frame): exactly one zombie check, never suspends. Returns the reaped pid,
   -ECHILD when no children remain, or -EAGAIN when children exist but none
   has exited yet. */
static uint64_t sys_waitpid_nb(uint64_t a1, uint64_t a2, uint64_t a3,
                               uint64_t a4, uint64_t a5)
{
    (void)a3; (void)a4; (void)a5;
    int pid_filter = (int)a1;
    int *status_out = (int *)a2;
    struct thread *t = sched_current();
    int self_pid = t ? t->pid : 0;

    if (status_out && !user_range_ok((uint64_t)status_out, sizeof(int), 1))
        return (uint64_t)-EFAULT;

    {
        struct thread *z = sched_child_zombie(self_pid, pid_filter);
        if (z)
        {
            int st = z->exit_status;
            int zpid = z->pid;
            sched_reap_zombie(z);
            if (status_out)
                *status_out = st;
            return (uint64_t)zpid;
        }
    }
    if (!sched_has_child(self_pid))
        return (uint64_t)(-ECHILD);
    return (uint64_t)(-EAGAIN);
}

static void con_putchar(char c)
{
    if (fb_active())
    {
        fb_putchar(c);
        fb_flush();
    }
    serial_putchar(COM1, c);
}

static uint64_t sys_write(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a4; (void)a5;
    int fd = (int)a1;
    const char *s = (const char *)a2;
    size_t len = (size_t)a3;
    if (len && !user_range_ok((uint64_t)s, len, 0))
        return (uint64_t)-EFAULT;
    vfs_ensure_proc();
    struct thread *t = sched_current();
    if (fd < 0 || fd >= MAX_FD || !t->fds[fd].used)
        return (uint64_t)-1;
    struct vfs_file *f = &t->fds[fd];
    if (f->kind == FD_TTY_IN)
        return (uint64_t)-1;
    if (f->kind == FD_CONSOLE_OUT)
    {
        for (size_t i = 0; i < len; i++)
            con_putchar(s[i]);
        return len;
    }
    if (f->kind == FD_PIPE)
        return sys_write_pipe(f, s, len);
    /* FD_VNODE */
    if (f->flags & O_APPEND)
        f->off = f->node->size;
    long w = vfs_write(f->node, f->off, s, len);
    if (w < 0)
        return (uint64_t)(size_t)w;   /* negative errno, kept as-is */
    f->off += (size_t)w;
    return (uint64_t)w;
}

static uint64_t sys_uname(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a2; (void)a3; (void)a4; (void)a5;
    struct utsname *u = (struct utsname *)a1;
    if (!u)
        return (uint64_t)-EFAULT;

    struct utsname k;
    strncpy(k.sysname, "axiomeKernel", sizeof(k.sysname));
    strncpy(k.nodename, "localhost", sizeof(k.nodename));
    strncpy(k.release, "0.1.0", sizeof(k.release));
#ifndef GIT_COMMIT
#define GIT_COMMIT "unknown"
#endif
#ifndef BUILD_TIME
#define BUILD_TIME "unknown"
#endif
    strncpy(k.version, "axiomeOS-" GIT_COMMIT "-" BUILD_TIME "-" __VERSION__,
            sizeof(k.version));
    strncpy(k.machine,
#ifdef __x86_64__
            "x86_64",
#else
            "unknown",
#endif
            sizeof(k.machine));
    k.domainname[0] = 0;

    long r = copy_to_user(u, &k, sizeof(k));
    return (uint64_t)r;
}

static uint64_t sys_mmap(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a5;
    int fd = (int)a1;
    uint64_t off = a2;
    uint64_t virt = a3;
    size_t len = a4;
    vfs_ensure_proc();
    struct thread *t = sched_current();
    if (fd < 0 || fd >= MAX_FD || !t->fds[fd].used)
        return (uint64_t)-1;
    struct vfs_file *f = &t->fds[fd];
    if (f->kind != FD_VNODE || !f->node)
        return (uint64_t)-1;
    return (uint64_t)vfs_mmap(f->node, off, virt, len, 0);
}

static uint64_t sys_write_pipe(struct vfs_file *f, const char *s, size_t len)
{
    if (f->node->type != VFS_PIPE_W)
        return (uint64_t)-1;
    struct pipe *p = (struct pipe *)f->node->priv;
    if (p->count + len > p->cap)
    {
        size_t ncap = p->cap ? p->cap : 256;
        while (ncap < p->count + len) ncap *= 2;
        uint8_t *nb = kmalloc(ncap);
        if (!nb) return 0;
        for (size_t i = 0; i < p->count; i++) nb[i] = p->buf[i];
        kfree(p->buf);
        p->buf = nb;
        p->cap = ncap;
    }
    for (size_t i = 0; i < len; i++)
        p->buf[p->count++] = (uint8_t)s[i];
    if (p->wait_reader)
    {
        sched_wake(p->wait_reader);
        p->wait_reader = 0;
    }
    return len;
}

static uint64_t sys_read(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a4; (void)a5;
    int fd = (int)a1;
    char *buf = (char *)a2;
    size_t len = (size_t)a3;
    if (len && !user_range_ok((uint64_t)buf, len, 1))
        return (uint64_t)-EFAULT;
    vfs_ensure_proc();
    struct thread *t = sched_current();
    if (fd < 0 || fd >= MAX_FD || !t->fds[fd].used)
        return (uint64_t)-1;
    struct vfs_file *f = &t->fds[fd];
    if (f->kind == FD_CONSOLE_OUT)
        return (uint64_t)-1;
    if (f->kind == FD_TTY_IN)
    {
        size_t got = 0;
        while (got < len)
        {
            char c;
            int r = tty_read_char_blocked(&c);
            if (r < 0)
            {
                /* Ctrl-C / signal interrupted the read. Return what we got;
                   the syscall dispatcher will deliver the signal. */
                if (got == 0)
                    return (uint64_t)(size_t)r;
                break;
            }
            buf[got++] = c;
        }
        return got;
    }
    if (f->kind == FD_PIPE)
    {
        if (f->node->type != VFS_PIPE_R)
            return (uint64_t)-1;
        struct pipe *p = (struct pipe *)f->node->priv;
        while (p->count == 0)
        {
            if (p->nwriters == 0)
                return 0; /* EOF */
            p->wait_reader = sched_current();
            sched_suspend();
        }
        size_t r = (len < p->count) ? len : p->count;
        for (size_t i = 0; i < r; i++)
            buf[i] = p->buf[i];
        for (size_t i = 0; i + r < p->count; i++)
            p->buf[i] = p->buf[i + r];
        p->count -= r;
        return r;
    }
    /* FD_VNODE */
    if (vfs_check_perms(f->node, VFS_MAY_READ) != 0)
        return (uint64_t)-EACCES;
    size_t r = vfs_read(f->node, f->off, buf, len);
    f->off += r;
    return r;
}

static int copy_path(uint64_t uptr, char *buf, size_t max)
{
    long n = copy_user_str(uptr, buf, max);
    if (n < 0)
        return 0;
    return (int)n;
}

#define EXEC_MAX_ARGS 32
#define EXEC_MAX_ENVS 32
#define EXEC_STR_MAX  256

static int copy_user_vec(char *const *uvec, int max, char store[][EXEC_STR_MAX],
                         char **out)
{
    if (!uvec)
        return 0;
    int count = 0;
    while (count < max)
    {
        uint64_t pp = (uint64_t)uvec + (uint64_t)count * sizeof(char *);
        if (!user_range_ok(pp, sizeof(char *), 0))
            return -1;
        uint64_t us = *(volatile uint64_t *)(uintptr_t)pp;
        if (!us)
            break;
        if (copy_user_str(us, store[count], EXEC_STR_MAX) < 0)
            return -1;
        out[count] = store[count];
        count++;
    }
    return count;
}

static int read_exec_file(const char *path, const char *cwd,
                          uint8_t **out_buf, size_t *out_size)
{
    struct vnode *n = vfs_lookup(path, cwd);
    if (!n || n->type != VFS_FILE)
    {
        if (n) vfs_release(n);
        return -1;
    }
    if (vfs_check_perms(n, VFS_MAY_EXEC) != 0)
    {
        vfs_release(n);
        return -1;
    }
    size_t sz = n->size;
    if (sz == 0)
    {
        vfs_release(n);
        return -1;
    }
    uint8_t *buf = kmalloc(sz);
    if (!buf)
    {
        vfs_release(n);
        return -1;
    }
    size_t done = 0;
    while (done < sz)
    {
        size_t r = vfs_read(n, done, buf + done, sz - done);
        if (r == 0) break;
        done += r;
    }
    vfs_release(n);
    if (done != sz)
    {
        kfree(buf);
        return -1;
    }
    *out_buf = buf;
    *out_size = sz;
    return 0;
}

static void make_abs(const char *path, const char *cwd, char *out, size_t outsz)
{
    if (path[0] == '/')
    {
        size_t l = 0;
        while (path[l] && l < outsz - 1) { out[l] = path[l]; l++; }
        out[l] = 0;
    }
    else
    {
        size_t l = 0;
        while (cwd[l] && l < outsz - 1) { out[l] = cwd[l]; l++; }
        if (l > 0 && out[l - 1] != '/')
        {
            if (l < outsz - 1) { out[l] = '/'; l++; }
        }
        out[l] = 0;
        size_t k = 0;
        while (path[k] && l < outsz - 1) { out[l] = path[k]; l++; k++; }
        out[l] = 0;
    }
    size_t len = 0;
    while (out[len]) len++;
    if (len > 1 && out[len - 1] == '/')
        out[len - 1] = 0;
    vfs_apply_aliases(out);
}

static uint64_t sys_open(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a3; (void)a4; (void)a5;
    char path[1024];
    if (copy_path(a1, path, sizeof(path)) == 0)
        return (uint64_t)-EFAULT;
    int flags = (int)a2;
    vfs_ensure_proc();
    struct thread *t = sched_current();

    struct vnode *n = vfs_lookup(path, t->cwd);
    if (n && n->type == VFS_FIFO)
    {
        struct pipe *p = (struct pipe *)n->priv;
        if (!p) { vfs_release(n); return (uint64_t)-EBADF; }
        int is_write = (flags & O_WRONLY) ? 1 : 0;
        struct vnode *end = kmalloc(sizeof(struct vnode));
        if (!end) { vfs_release(n); return (uint64_t)-ENOMEM; }
        memset(end, 0, sizeof(*end));
        end->type = is_write ? VFS_PIPE_W : VFS_PIPE_R;
        end->priv = p;
        end->refcount = 1;
        if (is_write) p->nwriters++; else p->nreaders++;
        vfs_release(n);
        int fd = fd_alloc(t);
        if (fd < 0)
        {
            if (is_write) p->nwriters--; else p->nreaders--;
            kfree(end);
            return (uint64_t)-ENOMEM;
        }
        t->fds[fd].used = 1; t->fds[fd].kind = FD_PIPE;
        t->fds[fd].node = end; t->fds[fd].off = 0;
        t->fds[fd].flags = flags;
        return (uint64_t)fd;
    }
    if (n)
    {
        if (n->type == VFS_DIR)
        {
            vfs_release(n);
            return (uint64_t)-EISDIR;
        }
        /* POSIX: validate the requested access mode against the file's mode
           at open time (issue #28/#32: read/write permissions used to be
           unchecked for opened FDs). */
        if (!(flags & O_WRONLY) && vfs_check_perms(n, VFS_MAY_READ) != 0)
        {
            vfs_release(n);
            return (uint64_t)-EACCES;
        }
        if ((flags & (O_WRONLY | O_RDWR)) &&
            vfs_check_perms(n, VFS_MAY_WRITE) != 0)
        {
            vfs_release(n);
            return (uint64_t)-EACCES;
        }
        if (flags & O_TRUNC)
        {
            if (n->data) kfree(n->data);
            n->data = 0;
            n->size = 0;
            n->cap = 0;
        }
    }
    else
    {
        if (!(flags & O_CREAT))
            return (uint64_t)-ENOENT;
        if (vfs_create(path, VFS_FILE, t->cwd) != 0)
            return (uint64_t)-EACCES;
        n = vfs_lookup(path, t->cwd);
        if (!n)
            return (uint64_t)-EIO;
    }

    int fd = fd_alloc(t);
    if (fd < 0)
    {
        vfs_release(n);
        return (uint64_t)-ENOMEM;
    }
    t->fds[fd].used = 1;
    t->fds[fd].kind = FD_VNODE;
    t->fds[fd].node = n;
    t->fds[fd].off = (flags & O_APPEND) ? n->size : 0;
    t->fds[fd].flags = flags;
    return (uint64_t)fd;
}

static uint64_t sys_close(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a2; (void)a3; (void)a4; (void)a5;
    int fd = (int)a1;
    vfs_ensure_proc();
    struct thread *t = sched_current();
    if (fd < 0 || fd >= MAX_FD || !t->fds[fd].used)
        return (uint64_t)-1;
    if (t->fds[fd].kind == FD_PIPE)
    {
        struct vnode *n = t->fds[fd].node;
        struct pipe *p = (struct pipe *)n->priv;
        if (n->type == VFS_PIPE_R) p->nreaders--;
        else p->nwriters--;
        if (n->refcount > 0) n->refcount--;
        if (n->refcount == 0) kfree(n);
        if (p->shared)
        {
            if (p->dead && p->nreaders == 0 && p->nwriters == 0)
            {
                if (p->buf) kfree(p->buf);
                kfree(p);
            }
        }
        else
        {
            if (p->nreaders == 0 && p->nwriters == 0)
            {
                if (p->buf) kfree(p->buf);
                kfree(p);
            }
            else if (p->wait_reader)
            {
                sched_wake(p->wait_reader);
                p->wait_reader = 0;
            }
        }
        t->fds[fd].used = 0;
        t->fds[fd].kind = FD_FREE;
        t->fds[fd].node = 0;
        t->fds[fd].off = 0;
        return 0;
    }
    if (t->fds[fd].kind == FD_VNODE)
        vfs_release(t->fds[fd].node);
    t->fds[fd].used = 0;
    t->fds[fd].kind = FD_FREE;
    t->fds[fd].node = 0;
    t->fds[fd].off = 0;
    return 0;
}

static uint64_t sys_mkdir(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a2; (void)a3; (void)a4; (void)a5;
    char path[1024];
    if (copy_path(a1, path, sizeof(path)) == 0)
        return (uint64_t)-EFAULT;
    vfs_ensure_proc();
    struct thread *t = sched_current();
    /* Check if it already exists */
    struct vnode *n = vfs_lookup(path, t->cwd);
    if (n) { vfs_release(n); return (uint64_t)-EEXIST; }
    if (vfs_create(path, VFS_DIR, t->cwd) != 0)
        return (uint64_t)-EACCES;
    return 0;
}

static uint64_t sys_unlink(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a2; (void)a3; (void)a4; (void)a5;
    char path[1024];
    if (copy_path(a1, path, sizeof(path)) == 0)
        return (uint64_t)-EFAULT;
    vfs_ensure_proc();
    struct thread *t = sched_current();
    /* Check existence and type first */
    struct vnode *n = vfs_lookup(path, t->cwd);
    if (!n)
        return (uint64_t)-ENOENT;
    if (n->type == VFS_DIR)
    {
        vfs_release(n);
        return (uint64_t)-EISDIR;
    }
    vfs_release(n);
    if (vfs_remove(path, t->cwd) != 0)
        return (uint64_t)-EACCES;
    return 0;
}

/* Verify a username/password pair against the kernel user database (issue
   #32: the libc sys_authenticate used to be an unused -1 stub). Returns the
   user's uid on success, -1 on bad credentials. */
static uint64_t sys_authenticate(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a3; (void)a4; (void)a5;
    char user[64], pass[128];
    if (copy_user_str(a1, user, sizeof(user)) < 0)
        return (uint64_t)-EFAULT;
    if (copy_user_str(a2, pass, sizeof(pass)) < 0)
        return (uint64_t)-EFAULT;
    return (uint64_t)security_authenticate(user, pass);
}

static uint64_t sys_rmdir(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a2; (void)a3; (void)a4; (void)a5;
    char path[1024];
    if (copy_path(a1, path, sizeof(path)) == 0)
        return (uint64_t)-EFAULT;
    vfs_ensure_proc();
    struct thread *t = sched_current();
    struct vnode *n = vfs_lookup(path, t->cwd);
    if (!n)
        return (uint64_t)-ENOENT;
    if (n->type != VFS_DIR)
    {
        vfs_release(n);
        return (uint64_t)-ENOTDIR;
    }
    /* Only an empty directory may be removed (issue #31: rm -r).
       The fs-level list includes "." and ".." (matching userspace readdir),
       so skip those when checking for remaining children. */
    struct vfs_dirent ents[8];
    long cnt = vfs_list(path, t->cwd, ents, 8);
    vfs_release(n);
    if (cnt < 0)
        return (uint64_t)-EIO;
    for (long i = 0; i < cnt; i++)
    {
        const char *nm = ents[i].name;
        if (nm[0] == '.' && (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0)))
            continue;
        return (uint64_t)-ENOTEMPTY;
    }
    if (vfs_remove(path, t->cwd) != 0)
        return (uint64_t)-EACCES;
    return 0;
}

static uint64_t sys_readdir(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a4; (void)a5;
    char path[1024];
    if (copy_path(a1, path, sizeof(path)) == 0)
        return (uint64_t)-EFAULT;
    struct vfs_dirent *uents = (struct vfs_dirent *)a2;
    int max = (int)a3;
    vfs_ensure_proc();
    struct thread *t = sched_current();

    /* Verify the path exists and is a directory */
    struct vnode *dn = vfs_lookup(path, t->cwd);
    if (!dn)
        return (uint64_t)-ENOENT;
    if (dn->type != VFS_DIR)
    {
        vfs_release(dn);
        return (uint64_t)-ENOTDIR;
    }
    vfs_release(dn);

    int cap = max;
    if (cap < 1) cap = 1;
    if (cap > 1024) cap = 1024;
    struct vfs_dirent *kbuf = kmalloc(sizeof(struct vfs_dirent) * cap);
    if (!kbuf)
        return (uint64_t)-ENOMEM;
    int count = vfs_list(path, t->cwd, kbuf, cap);
    if (count < 0)
    {
        kfree(kbuf);
        return (uint64_t)-EIO;
    }
    int tocopy = count;
    if (tocopy > max) tocopy = max;
    if (tocopy > cap) tocopy = cap;
    if (tocopy > 0)
    {
        long r = copy_to_user(uents, kbuf,
                              (size_t)tocopy * sizeof(struct vfs_dirent));
        if (r < 0)
        {
            kfree(kbuf);
            return (uint64_t)r;
        }
    }
    kfree(kbuf);
    return (uint64_t)count;
}

static uint64_t sys_chdir(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a2; (void)a3; (void)a4; (void)a5;
    char path[1024];
    if (copy_path(a1, path, sizeof(path)) == 0)
        return (uint64_t)-EFAULT;
    vfs_ensure_proc();
    struct thread *t = sched_current();
    struct vnode *n = vfs_lookup(path, t->cwd);
    if (!n)
        return (uint64_t)-ENOENT;
    if (n->type != VFS_DIR)
    {
        vfs_release(n);
        return (uint64_t)-ENOTDIR;
    }
    char abs[1024];
    make_abs(path, t->cwd, abs, sizeof(abs));
    int i = 0;
    while (abs[i] && i < 255) { t->cwd[i] = abs[i]; i++; }
    t->cwd[i] = 0;
    vfs_release(n);
    return 0;
}

static uint64_t sys_getcwd(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a3; (void)a4; (void)a5;
    char *ubuf = (char *)a1;
    size_t sz = (size_t)a2;
    vfs_ensure_proc();
    struct thread *t = sched_current();
    size_t l = 0;
    while (t->cwd[l]) l++;
    if (sz < l + 1)
        return (uint64_t)-1;
    return (uint64_t)copy_to_user(ubuf, t->cwd, l + 1);
}

static uint64_t sys_fstat(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a3; (void)a4; (void)a5;
    int fd = (int)a1;
    struct stat *ust = (struct stat *)a2;
    if (!ust)
        return (uint64_t)-EFAULT;
    vfs_ensure_proc();
    struct thread *t = sched_current();
    if (fd < 0 || fd >= MAX_FD || !t->fds[fd].used)
        return (uint64_t)-EBADF;
    struct vfs_file *f = &t->fds[fd];
    if (f->kind != FD_VNODE || !f->node)
        return (uint64_t)-EBADF;
    struct stat st;
    st.st_size = (uint64_t)f->node->size;
    st.st_mode = (f->node->type == VFS_DIR) ? S_IFDIR : S_IFREG;
    st.st_nlink = 1;
    st.st_ino = (uint32_t)(uintptr_t)f->node->priv;
    return (uint64_t)copy_to_user(ust, &st, sizeof(st));
}

/* Path-based stat() (issue #28). Unlike the stat() helper in libc, this does
   not require opening the file first, so it works on directories and on
   files that cannot be opened read-only. */
static uint64_t sys_stat(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a3; (void)a4; (void)a5;
    char path[1024];
    if (copy_path(a1, path, sizeof(path)) == 0)
        return (uint64_t)-EFAULT;
    struct stat *ust = (struct stat *)a2;
    if (!ust)
        return (uint64_t)-EFAULT;
    vfs_ensure_proc();
    struct thread *t = sched_current();
    struct vnode *n = vfs_lookup(path, t->cwd);
    if (!n)
        return (uint64_t)-ENOENT;
    if (vfs_check_perms(n, VFS_MAY_READ) != 0)
    {
        vfs_release(n);
        return (uint64_t)-EACCES;
    }
    struct stat st;
    st.st_size = (uint64_t)n->size;
    st.st_mode = (n->type == VFS_DIR) ? S_IFDIR : S_IFREG;
    st.st_nlink = 1;
    st.st_ino = (uint32_t)(uintptr_t)n->priv;
    vfs_release(n);
    return (uint64_t)copy_to_user(ust, &st, sizeof(st));
}

static uint64_t sys_dup2(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a3; (void)a4; (void)a5;
    int oldfd = (int)a1;
    int newfd = (int)a2;
    vfs_ensure_proc();
    struct thread *t = sched_current();
    if (oldfd < 0 || oldfd >= MAX_FD || !t->fds[oldfd].used)
        return (uint64_t)-1;
    if (newfd < 0 || newfd >= MAX_FD)
        return (uint64_t)-1;
    if (oldfd == newfd)
        return (uint64_t)newfd;
    if (t->fds[newfd].used)
    {
        uint64_t r = sys_close((uint64_t)newfd, 0, 0, 0, 0);
        if (r == (uint64_t)-1)
            return (uint64_t)-1;
    }
    struct vfs_file *o = &t->fds[oldfd];
    struct vfs_file *n = &t->fds[newfd];
    n->used = 1;
    n->kind = o->kind;
    n->node = o->node;
    n->off = o->off;
    n->flags = o->flags;
    if (o->kind == FD_VNODE && o->node)
        o->node->refcount++;
    else if (o->kind == FD_PIPE && o->node)
    {
        struct pipe *p = (struct pipe *)o->node->priv;
        if (o->node->type == VFS_PIPE_R) p->nreaders++;
        else p->nwriters++;
        o->node->refcount++;
    }
    return (uint64_t)newfd;
}

static uint64_t sys_pipe(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a2; (void)a3; (void)a4; (void)a5;
    int *ufds = (int *)a1;
    if (!user_range_ok((uint64_t)ufds, 2 * sizeof(int), 1))
        return (uint64_t)-EFAULT;
    vfs_ensure_proc();
    struct thread *t = sched_current();
    struct pipe *p = kmalloc(sizeof(*p));
    if (!p) return (uint64_t)-1;
    p->buf = kmalloc(256);
    if (!p->buf) { kfree(p); return (uint64_t)-1; }
    p->cap = 256;
    p->count = 0;
    p->nreaders = 1;
    p->nwriters = 1;
    p->wait_reader = 0;
    p->shared = 0;
    p->dead = 0;

    struct vnode *r = kmalloc(sizeof(struct vnode));
    struct vnode *w = kmalloc(sizeof(struct vnode));
    if (!r || !w) { kfree(p->buf); kfree(p); kfree(r); kfree(w); return (uint64_t)-1; }
    memset(r, 0, sizeof(*r));
    memset(w, 0, sizeof(*w));
    r->type = VFS_PIPE_R; r->priv = p; r->refcount = 1;
    w->type = VFS_PIPE_W; w->priv = p; w->refcount = 1;

    int rfd = fd_alloc(t);
    if (rfd < 0) { kfree(p->buf); kfree(p); kfree(r); kfree(w); return (uint64_t)-1; }
    t->fds[rfd].used = 1;
    int wfd = fd_alloc(t);
    if (wfd < 0) { t->fds[rfd].used = 0; kfree(p->buf); kfree(p); kfree(r); kfree(w); return (uint64_t)-1; }
    t->fds[rfd].kind = FD_PIPE; t->fds[rfd].node = r; t->fds[rfd].off = 0;
    t->fds[wfd].used = 1; t->fds[wfd].kind = FD_PIPE; t->fds[wfd].node = w; t->fds[wfd].off = 0;

    ufds[0] = rfd;
    ufds[1] = wfd;
    return 0;
}

static uint64_t sys_mkfifo(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a2; (void)a3; (void)a4; (void)a5;
    char path[1024];
    if (copy_path(a1, path, sizeof(path)) == 0)
        return (uint64_t)-EFAULT;
    vfs_ensure_proc();
    struct thread *t = sched_current();
    struct vnode *n = vfs_lookup(path, t->cwd);
    if (n) { vfs_release(n); return (uint64_t)-EEXIST; }
    if (vfs_create(path, VFS_FIFO, t->cwd) != 0)
        return (uint64_t)-EACCES;
    return 0;
}

static uint64_t sys_drv_rescan(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    driver_rescan();
    return 0;
}

static uint64_t sys_mount(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a4; (void)a5;
    int fstype = (int)a1;
    char mp[1024];
    if (copy_path(a2, mp, sizeof(mp)) == 0)
        return (uint64_t)-1;
    int dev = (int)a3;
    vfs_ensure_proc();
    if (fstype == FS_RAMFS)
        return (uint64_t)vfs_mount_ramfs(mp);
    if (fstype == FS_FAT32)
    {
        int bus = (dev >> 16) & 0xFF;
        int drive = (dev >> 8) & 0xFF;
        int part = dev & 0xFF;
        return (uint64_t)fat32_mount_part(bus, drive, part, mp);
    }
    if (fstype == FS_AXIOMEFS)
    {
        int bus = (dev >> 16) & 0xFF;
        int drive = (dev >> 8) & 0xFF;
        int part = dev & 0xFF;
        return (uint64_t)axiomefs_mount_part(bus, drive, part, mp);
    }
    return (uint64_t)-1;
}

static uint64_t sys_umount(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a2; (void)a3; (void)a4; (void)a5;
    char mp[1024];
    if (copy_path(a1, mp, sizeof(mp)) == 0)
        return (uint64_t)-1;
    vfs_ensure_proc();
    return (uint64_t)vfs_umount(mp);
}

void vfs_ensure_proc(void)
{
    struct thread *t = sched_current();
    if (!t)
        return;
    if (t->fds[0].used && t->fds[1].used && t->fds[2].used)
    {
        if (t->cwd[0] == 0) { t->cwd[0] = '/'; t->cwd[1] = 0; }
        return;
    }
    t->fds[0].used = 1; t->fds[0].kind = FD_TTY_IN;      t->fds[0].node = 0; t->fds[0].off = 0;
    t->fds[1].used = 1; t->fds[1].kind = FD_CONSOLE_OUT; t->fds[1].node = 0; t->fds[1].off = 0;
    t->fds[2].used = 1; t->fds[2].kind = FD_CONSOLE_OUT; t->fds[2].node = 0; t->fds[2].off = 0;
    if (t->cwd[0] == 0) { t->cwd[0] = '/'; t->cwd[1] = 0; }
}

static int try_exec_path(const char *path, int argc, char **argv)
{
    uint8_t *buf = 0;
    size_t sz = 0;
    if (vfs_read_file(path, &buf, &sz) != 0)
        return -1;
    int pid = spawn_process_with_args(buf, sz, path, argc, argv);
    kfree(buf);
    return pid;
}

static uint64_t sys_execve(uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5)
{
    (void)a4; (void)a5;
    char path[1024];
    if (copy_path(a1, path, sizeof(path)) == 0)
        return (uint64_t)-1;

    char argv_store[EXEC_MAX_ARGS][EXEC_STR_MAX];
    char env_store[EXEC_MAX_ENVS][EXEC_STR_MAX];
    char *argv[EXEC_MAX_ARGS];
    char *envp[EXEC_MAX_ENVS];
    int argc = copy_user_vec((char *const *)a2, EXEC_MAX_ARGS, argv_store, argv);
    int envc = copy_user_vec((char *const *)a3, EXEC_MAX_ENVS, env_store, envp);

    if (argc < 0 || envc < 0)
        return (uint64_t)-EFAULT;

    if (argc == 0)
    {
        argv_store[0][0] = 0;
        size_t i = 0;
        while (path[i] && i < EXEC_STR_MAX - 1)
        {
            argv_store[0][i] = path[i];
            i++;
        }
        argv_store[0][i] = 0;
        argv[0] = argv_store[0];
        argc = 1;
    }

    vfs_ensure_proc();
    struct thread *t = sched_current();
    if (!t)
        return (uint64_t)-1;

    uint8_t *buf = 0;
    size_t sz = 0;
    if (read_exec_file(path, t->cwd, &buf, &sz) != 0)
        return (uint64_t)-1;

    struct mmu_root *mmu = vmm_new_user_root();
    if (!mmu)
    {
        kfree(buf);
        return (uint64_t)-1;
    }
    uint64_t entry = 0;
    uint64_t stack_top = 0;
    if (elf_load(mmu, buf, sz, &entry, &stack_top, argc, argv, envc, envp) < 0)
    {
        vmm_free_root(mmu);
        kfree(buf);
        return (uint64_t)-1;
    }
    kfree(buf);

    for (int i = 0; i < NSIG; i++)
    {
        if (t->sig_actions[i].sa_handler != SIG_IGN)
            t->sig_actions[i].sa_handler = SIG_DFL;
        t->sig_actions[i].sa_flags = 0;
        t->sig_actions[i].sa_restorer = 0;
        t->sig_actions[i].sa_mask = 0;
    }
    t->sig_pending = 0;

    if (sched_set_current_image(mmu, (void *)entry, (void *)stack_top,
                                syscall_uregs.rflags, path) != 0)
    {
        vmm_free_root(mmu);
        return (uint64_t)-1;
    }
    return 0;
}

static const char *kpath_dirs[] = { "/", "/bin", "/Binaries", 0 };

static uint64_t sys_spawn_cmd(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a3; (void)a4; (void)a5;
    const char *u_cmd = (const char *)a1;
    size_t len = (size_t)a2;
    if (len > 255)
        len = 255;
    if (len && !user_range_ok((uint64_t)u_cmd, len, 0))
        return (uint64_t)-EFAULT;
    char cmd[256];
    for (size_t i = 0; i < len; i++)
        cmd[i] = u_cmd[i];
    cmd[len] = 0;

    char *argv[16];
    int argc = 0;
    char *p = cmd;
    while (*p && argc < 16)
    {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        if (*p)
        {
            *p = 0;
            p++;
        }
    }
    if (argc == 0)
        return (uint64_t)(-1);

    {
        const char *p = argv[0];
        int has_slash = 0;
        while (*p) { if (*p == '/') { has_slash = 1; break; } p++; }
        if (has_slash)
        {
            int pid = try_exec_path(argv[0], argc, argv);
            if (pid >= 0)
                return (uint64_t)pid;
            printk("SPAWN_CMD: cannot exec '%s'\n", argv[0]);
            return (uint64_t)(-1);
        }
    }

    for (int d = 0; kpath_dirs[d]; d++)
    {
        size_t dl = strlen(kpath_dirs[d]);
        size_t nl = strlen(argv[0]);
        if (dl + 1 + nl >= 256)
            continue;
        char full[256];
        memcpy(full, kpath_dirs[d], dl);
        size_t p = dl;
        if (p > 0 && kpath_dirs[d][dl - 1] != '/')
            full[p++] = '/';
        memcpy(full + p, argv[0], nl + 1);
        int pid = try_exec_path(full, argc, argv);
        if (pid >= 0)
            return (uint64_t)pid;
    }

    printk("SPAWN_CMD: unknown program '%s'\n", argv[0]);
    return (uint64_t)(-1);
}

static uint64_t sys_ps(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a3; (void)a4; (void)a5;
    struct proc_info *ubuf = (struct proc_info *)a1;
    int max = (int)a2;
    if (max > 64)
        max = 64;
    if (max <= 0 || !ubuf)
        return 0;
    
    struct proc_info kbuf[64];
    int n = sched_enum_procs(kbuf, max);
    if (n < 0) n = 0;
    
    if (n > 0)
    {
        long r = copy_to_user(ubuf, kbuf, (size_t)n * sizeof(struct proc_info));
        if (r < 0)
            return (uint64_t)r;
    }
    
    return (uint64_t)n;
}

/* ---- sockets (Phase 13) ---- */

static uint64_t sys_socket_create(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a3; (void)a4; (void)a5;
    return (uint64_t)sock_create((int)a1, (int)a2, 0);
}

static uint64_t sys_socket_bind(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a4; (void)a5;
    int addrlen = (int)a3;
    if (addrlen < (int)sizeof(struct sockaddr_in))
        return (uint64_t)-1;
    struct sockaddr_in sin;
    if (copy_from_user(&sin, (const void *)a2, sizeof(sin)) < 0)
        return (uint64_t)-EFAULT;
    return (uint64_t)sock_bind((int)a1, (const struct sockaddr *)&sin, addrlen);
}

static uint64_t sys_socket_connect(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a4; (void)a5;
    int addrlen = (int)a3;
    if (addrlen < (int)sizeof(struct sockaddr_in))
        return (uint64_t)-1;
    struct sockaddr_in sin;
    if (copy_from_user(&sin, (const void *)a2, sizeof(sin)) < 0)
        return (uint64_t)-EFAULT;
    return (uint64_t)sock_connect((int)a1, (const struct sockaddr *)&sin, addrlen);
}

static uint64_t sys_socket_send(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a4; (void)a5;
    size_t len = (size_t)a3;
    if (len && !user_range_ok((uint64_t)a2, len, 0))
        return (uint64_t)-EFAULT;
    return (uint64_t)sock_send((int)a1, (const void *)a2, len);
}

static uint64_t sys_socket_recv(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a4; (void)a5;
    size_t max = (size_t)a3;
    if (max == 0)
        return 0;
    if (max > 65536)
        max = 65536;
    uint8_t *kbuf = kmalloc(max);
    if (!kbuf)
        return (uint64_t)-1;
    long got = sock_recv((int)a1, kbuf, max);
    if (got < 0)
    {
        kfree(kbuf);
        return (uint64_t)-1;
    }
    long r = copy_to_user((void *)a2, kbuf, (size_t)got);
    kfree(kbuf);
    if (r < 0)
        return (uint64_t)r;
    return (uint64_t)got;
}

static uint64_t sys_socket_close(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a2; (void)a3; (void)a4; (void)a5;
    return (uint64_t)sock_close((int)a1);
}

static uint64_t sys_socket_listen(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a2; (void)a3; (void)a4; (void)a5;
    return (uint64_t)sock_listen((int)a1);
}

static uint64_t sys_socket_accept(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a2; (void)a3; (void)a4; (void)a5;
    return (uint64_t)sock_accept((int)a1);
}

/* ---- DNS / ICMP services (Phase 13) ---- */

static uint64_t sys_dns_resolve(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a3; (void)a4; (void)a5;
    char name[256];
    long r = copy_user_str(a1, name, sizeof(name));
    if (r < 0)
        return (uint64_t)r;
    ip4_addr_t ip = 0;
    if (dns_resolve(netdev_up(), name, &ip) != 0)
        return (uint64_t)-1;
    if (a2)
    {
        r = copy_to_user((void *)a2, &ip, sizeof(ip));
        if (r < 0)
            return (uint64_t)r;
    }
    return 0;
}

static uint64_t sys_ping(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a4; (void)a5;
    uint32_t rtt = 0;
    int rc = icmp_echo((ip4_addr_t)(uint32_t)a1, (uint32_t)a2, &rtt);
    if (rc != 0)
        return (uint64_t)-1;
    if (a3)
    {
        long r2 = copy_to_user((void *)a3, &rtt, sizeof(rtt));
        if (r2 < 0)
            return (uint64_t)r2;
    }
    return 0;
}

/* ---- signals (Phase 11) ---- */

static uint64_t sys_sigaction(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a4; (void)a5;
    int sig = (int)a1;
    const struct sigaction *act = (const struct sigaction *)a2;
    struct sigaction *oldact = (struct sigaction *)a3;
    if (sig <= 0 || sig >= NSIG)
        return (uint64_t)(-1);
    if (sig == SIGKILL || sig == SIGSTOP)
        return (uint64_t)(-1);
    struct thread *t = sched_current();
    if (!t)
        return (uint64_t)(-1);

    struct sigaction kact;
    if (act)
    {
        if (copy_from_user(&kact, act, sizeof(kact)) < 0)
            return (uint64_t)-EFAULT;
    }
    if (oldact)
    {
        if (copy_to_user(oldact, &t->sig_actions[sig], sizeof(*oldact)) < 0)
            return (uint64_t)-EFAULT;
    }
    if (act)
        t->sig_actions[sig] = kact;
    return 0;
}

struct kill_arg { int sig; int self; };
static void kill_fn(struct thread *t, void *arg)
{
    struct kill_arg *ka = (struct kill_arg *)arg;
    if (t->pid == ka->self)
        return;
    if (sched_is_protected(t))
        return;
    t->sig_pending |= (1ULL << ka->sig);
}

static uint64_t sys_kill(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a3; (void)a4; (void)a5;
    int pid = (int)a1;
    int sig = (int)a2;
    if (sig <= 0 || sig >= NSIG)
        return (uint64_t)(-1);
    struct thread *self = sched_current();
    if (self)
    {
        if (pid > 0)
        {
            struct thread *t = sched_find_by_pid(pid);
            if (!t)
                return (uint64_t)(-ESRCH);
            int pr = sched_protected_kill(t, self);
            if (pr != 0)
                return (uint64_t)pr;
            if (t->uid != self->euid &&
                self->role != ROLE_SYSTEM &&
                !(self->caps_prm & (CAP_KILL_ANY | CAP_SIGNAL_OTHER)))
                return (uint64_t)(-EPERM);
            t->sig_pending |= (1ULL << sig);
            return 0;
        }
        if (pid == 0)
        {
            self->sig_pending |= (1ULL << sig);
            return 0;
        }
        if (pid == -1)
        {
            if (self->role != ROLE_SYSTEM && !(self->caps_prm & CAP_KILL_ANY))
                return (uint64_t)(-EPERM);
            struct kill_arg ka = { sig, self->pid };
            sched_foreach(kill_fn, &ka);
            return 0;
        }
        return (uint64_t)(-1);
    }
    return (uint64_t)(-1);
}

static uint64_t sys_sigreturn(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    struct thread *t = sched_current();
    if (!t || !t->syscall_iret)
        return (uint64_t)(-1);
    uint64_t *frame = t->syscall_iret;
    uint64_t user_rsp = frame[3];
    if (!user_range_ok(user_rsp, sizeof(struct sigframe), 0))
        return (uint64_t)-EFAULT;
    struct sigframe *sf = (struct sigframe *)user_rsp;
    frame[-1]  = sf->rax;
    frame[-2]  = sf->rdi;
    frame[-3]  = sf->rsi;
    frame[-4]  = sf->rdx;
    frame[-5]  = sf->r10;
    frame[-6]  = sf->r8;
    frame[-7]  = sf->r9;
    frame[-8]  = sf->rbx;
    frame[-9]  = sf->rbp;
    frame[-10] = sf->r12;
    frame[-11] = sf->r13;
    frame[-12] = sf->r14;
    frame[-13] = sf->r15;
    frame[0] = sf->rip;
    frame[1] = sf->cs;
    frame[2] = sf->rflags;
    frame[3] = sf->rsp;
    frame[4] = sf->ss;
    t->sig_mask = sf->oldmask;
    return 0;
}

/* ================================================================ *
 * User rank system: identity / capability syscalls
 * ================================================================ */

static uint64_t sys_getuid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a1;(void)a2;(void)a3;(void)a4;(void)a5;
    struct thread *t = sched_current();
    return (uint64_t)(t ? t->uid : 0);
}

static uint64_t sys_geteuid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a1;(void)a2;(void)a3;(void)a4;(void)a5;
    struct thread *t = sched_current();
    return (uint64_t)(t ? t->euid : 0);
}

static uint64_t sys_getgid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a1;(void)a2;(void)a3;(void)a4;(void)a5;
    struct thread *t = sched_current();
    return (uint64_t)(t ? t->gid : 0);
}

static uint64_t sys_getegid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a1;(void)a2;(void)a3;(void)a4;(void)a5;
    struct thread *t = sched_current();
    return (uint64_t)(t ? t->egid : 0);
}

static uint64_t sys_getrole(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a1;(void)a2;(void)a3;(void)a4;(void)a5;
    struct thread *t = sched_current();
    return (uint64_t)(t ? (int)t->role : 0);
}

static uint64_t sys_getcap(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a1;(void)a2;(void)a3;(void)a4;(void)a5;
    struct thread *t = sched_current();
    return (uint64_t)(t ? t->caps_eff : 0);
}

static uint64_t sys_setuid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a2;(void)a3;(void)a4;(void)a5;
    struct thread *t = sched_current();
    if (!t)
        return (uint64_t)-EPERM;
    if (t->role != ROLE_SYSTEM && !(t->caps_prm & CAP_SETUID))
        return (uint64_t)-EPERM;
    t->uid  = (uid_t)a1;
    t->euid = (uid_t)a1;
    t->suid = (uid_t)a1;
    t->role = posix_uid_to_role((uid_t)a1);
    t->caps_eff = t->caps_prm = t->caps_inh = role_caps[t->role];
    printk("SEC: pid %d setuid -> uid=%lu role=%d\n", t->pid, (unsigned long)a1, t->role);
    return 0;
}

static uint64_t sys_setgid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a2;(void)a3;(void)a4;(void)a5;
    struct thread *t = sched_current();
    if (!t)
        return (uint64_t)-EPERM;
    if (t->role != ROLE_SYSTEM && !(t->caps_prm & CAP_SETGID))
        return (uint64_t)-EPERM;
    t->gid  = (gid_t)a1;
    t->egid = (gid_t)a1;
    t->sgid = (gid_t)a1;
    return 0;
}

static uint64_t sys_setcap(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a2;(void)a3;(void)a4;(void)a5;
    struct thread *t = sched_current();
    if (!t)
        return (uint64_t)-EPERM;
    if (t->role != ROLE_SYSTEM && !(t->caps_prm & CAP_SYS_ADMIN))
        return (uint64_t)-EPERM;
    t->caps_eff = (uint64_t)a1 & t->caps_prm;
    return 0;
}

static uint64_t sys_getpwnam(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a4;(void)a5;
    uid_t *uout = (uid_t *)a2;
    gid_t *gout = (gid_t *)a3;
    char name[64];
    if (copy_user_str(a1, name, sizeof(name)) < 0)
        return (uint64_t)-EFAULT;
    const struct user_entry *u = security_lookup_name(name);
    if (!u) return (uint64_t)-1;
    if (uout)
    {
        if (copy_to_user(uout, &u->uid, sizeof(u->uid)) < 0)
            return (uint64_t)-EFAULT;
    }
    if (gout)
    {
        if (copy_to_user(gout, &u->gid, sizeof(u->gid)) < 0)
            return (uint64_t)-EFAULT;
    }
    return 0;
}

static uint64_t sys_reload_users(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a1;(void)a2;(void)a3;(void)a4;(void)a5;
    struct thread *t = sched_current();
    if (!t)
        return (uint64_t)-EPERM;
    if (t->role != ROLE_SYSTEM && !(t->caps_prm & CAP_USER_MGMT))
        return (uint64_t)-EPERM;
    security_reload();
    return 0;
}

/* ---- loadable kernel modules (.kxt) ---- */

static int module_privileged(void)
{
    struct thread *t = sched_current();
    if (!t)
        return 1;   /* kernel thread */
    if (t->role == ROLE_SYSTEM)
        return 1;
    return (t->caps_prm & CAP_SYS_ADMIN) ? 1 : 0;
}

static uint64_t sys_module_load(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a2; (void)a3; (void)a4; (void)a5;
    if (!module_privileged())
        return (uint64_t)(-EPERM);
    char path[256];
    if (copy_user_str(a1, path, sizeof(path)) < 0)
        return (uint64_t)-EFAULT;
    return (uint64_t)module_load_file(path);
}

static uint64_t sys_module_unload(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a2; (void)a3; (void)a4; (void)a5;
    if (!module_privileged())
        return (uint64_t)(-EPERM);
    char name[64];
    if (copy_user_str(a1, name, sizeof(name)) < 0)
        return (uint64_t)-EFAULT;
    return (uint64_t)module_unload(name);
}

static uint64_t sys_chmod(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a3;(void)a4;(void)a5;
    char path[1024];
    if (copy_path(a1, path, sizeof(path)) == 0)
        return (uint64_t)-EFAULT;
    struct thread *t = sched_current();
    if (!t) return (uint64_t)-EPERM;
    vfs_ensure_proc();
    struct vnode *n = vfs_lookup(path, t->cwd);
    if (!n) return (uint64_t)-ENOENT;
    if (n->v_uid != t->euid && t->role != ROLE_SYSTEM && !(t->caps_prm & CAP_SYS_ADMIN))
    {
        vfs_release(n);
        return (uint64_t)-EPERM;
    }
    n->v_mode = (uint16_t)((uint64_t)a2 & 07777);
    vfs_release(n);
    return 0;
}

static uint64_t sys_chown(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a4;(void)a5;
    char path[1024];
    if (copy_path(a1, path, sizeof(path)) == 0)
        return (uint64_t)-EFAULT;
    struct thread *t = sched_current();
    if (!t) return (uint64_t)-EPERM;
    vfs_ensure_proc();
    struct vnode *n = vfs_lookup(path, t->cwd);
    if (!n) return (uint64_t)-ENOENT;
    if (n->v_uid != t->euid && t->role != ROLE_SYSTEM && !(t->caps_prm & CAP_SYS_ADMIN))
    {
        vfs_release(n);
        return (uint64_t)-EPERM;
    }
    n->v_uid = (uid_t)a2;
    n->v_gid = (gid_t)a3;
    vfs_release(n);
    return 0;
}

/* Core signal delivery against an explicit user register context (`ctx`,
   whose rip/cs/rflags/rsp/ss are the interrupted user frame and whose GP
   fields are the interrupted user registers). Rewrites `ctx` in place to
   point at the handler. Returns non-zero if a signal was consumed (the caller
   must write `ctx` back). For SIGKILL/default signals the process is torn
   down (sched_exit does not return). */
static int deliver_signal_ctx(struct thread *t, struct sigframe *ctx)
{
    uint64_t pending = t->sig_pending & ~t->sig_mask;
    if (!pending)
        return 0;
    int sig = 0;
    for (int i = 1; i < NSIG; i++)
        if (pending & ((uint64_t)1 << i)) { sig = i; break; }
    if (!sig)
        return 0;

    t->sig_pending &= ~((uint64_t)1 << sig);
    struct sigaction *sa = &t->sig_actions[sig];

    if (sig == SIGKILL) { sched_exit(137); return 1; }

    if (sa->sa_handler == SIG_DFL)
    {
        if (sig != SIGCHLD && sig != SIGCONT)
            sched_exit(128 + sig);
        return 1;
    }
    if (sa->sa_handler == SIG_IGN)
        return 1;
    if (!sa->sa_restorer)
        return 1;

    uint64_t u_rsp = ctx->rsp;
    size_t fsz = sizeof(struct sigframe);
    uint64_t new_rsp = u_rsp - fsz - 8;

    /* The frame is written straight to the user stack; if it would land in an
       unmapped page (e.g. a full stack) the process gets SIGSEGV rather than
       the kernel corrupting memory. */
    if (!user_range_ok(new_rsp, fsz + 8, 1))
    {
        sched_exit(139);
        return 1;
    }
    struct sigframe *sf = (struct sigframe *)(new_rsp + 8);
    *sf = *ctx;
    sf->oldmask = t->sig_mask;

    *(uint64_t *)new_rsp = (uint64_t)sa->sa_restorer;

    t->sig_mask |= (1ULL << sig);

    ctx->rip = (uint64_t)sa->sa_handler;
    ctx->rsp = new_rsp;
    ctx->rdi = (uint64_t)sig;
    return 1;
}

void syscall_deliver_signals(void)
{
    struct thread *t = sched_current();
    if (!t || !t->syscall_iret)
        return;
    uint64_t *frame = t->syscall_iret;
    struct sigframe ctx;
    ctx.rax=frame[-1]; ctx.rdi=frame[-2]; ctx.rsi=frame[-3]; ctx.rdx=frame[-4];
    ctx.r10=frame[-5]; ctx.r8=frame[-6];  ctx.r9=frame[-7];  ctx.rbx=frame[-8];
    ctx.rbp=frame[-9]; ctx.r12=frame[-10]; ctx.r13=frame[-11]; ctx.r14=frame[-12]; ctx.r15=frame[-13];
    ctx.rip=frame[0]; ctx.cs=frame[1]; ctx.rflags=frame[2]; ctx.rsp=frame[3]; ctx.ss=frame[4];
    if (deliver_signal_ctx(t, &ctx))
    {
        frame[-1]=ctx.rax; frame[-2]=ctx.rdi; frame[-3]=ctx.rsi; frame[-4]=ctx.rdx;
        frame[-5]=ctx.r10; frame[-6]=ctx.r8;  frame[-7]=ctx.r9;  frame[-8]=ctx.rbx;
        frame[-9]=ctx.rbp; frame[-10]=ctx.r12; frame[-11]=ctx.r13; frame[-12]=ctx.r14; frame[-13]=ctx.r15;
        frame[0]=ctx.rip; frame[1]=ctx.cs; frame[2]=ctx.rflags; frame[3]=ctx.rsp; frame[4]=ctx.ss;
    }
}

/* Deliver pending signals to the current thread when a device IRQ (the timer)
   interrupted it in user mode, so SIGINT/SIGKILL break a CPU-bound loop that
   makes no syscalls (issue #30). `ptr` is the raw isr_frame from isr.S. */
void kernel_deliver_signals_user(void *ptr)
{
    struct isr_frame *f = (struct isr_frame *)ptr;
    struct thread *t = sched_current();
    if (!t || !f)
        return;
    if ((f->cs & 3) == 0)
        return;   /* interrupted kernel mode: the syscall-return path handles it */
    struct sigframe ctx;
    ctx.rax=f->rax; ctx.rdi=f->rdi; ctx.rsi=f->rsi; ctx.rdx=f->rdx;
    ctx.r10=f->r10; ctx.r8=f->r8;   ctx.r9=f->r9;   ctx.rbx=f->rbx;
    ctx.rbp=f->rbp; ctx.r12=f->r12; ctx.r13=f->r13; ctx.r14=f->r14; ctx.r15=f->r15;
    ctx.rip=f->rip; ctx.cs=f->cs; ctx.rflags=f->rflags; ctx.rsp=f->rsp; ctx.ss=f->ss;
    if (deliver_signal_ctx(t, &ctx))
    {
        f->rax=ctx.rax; f->rdi=ctx.rdi; f->rsi=ctx.rsi; f->rdx=ctx.rdx;
        f->r10=ctx.r10; f->r8=ctx.r8;   f->r9=ctx.r9;   f->rbx=ctx.rbx;
        f->rbp=ctx.rbp; f->r12=ctx.r12; f->r13=ctx.r13; f->r14=ctx.r14; f->r15=ctx.r15;
        f->rip=ctx.rip; f->cs=ctx.cs; f->rflags=ctx.rflags; f->rsp=ctx.rsp; f->ss=ctx.ss;
    }
}


/* ---- IPC channels (Phase 11) ---- */
#define IPC_CHANS 16
#define IPC_MSG_MAX 32
#define IPC_MSG_LEN 256

struct ipc_msg {
    uint8_t data[IPC_MSG_LEN];
    size_t len;
};

struct ipc_chan {
    int id;
    int used;
    struct ipc_msg msgs[IPC_MSG_MAX];
    int count;
    int head;
    struct thread *wait_recv;
};

static struct ipc_chan g_ipc[IPC_CHANS];
static int g_ipc_next_id;

static uint64_t sys_ipc_create(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    struct thread *t = sched_current();
    if (!t)
        return (uint64_t)(-1);
    int c = -1;
    for (int i = 0; i < IPC_CHANS; i++)
        if (!g_ipc[i].used) { c = i; break; }
    if (c < 0)
        return (uint64_t)(-1);
    g_ipc[c].used = 1;
    g_ipc[c].id = ++g_ipc_next_id;
    g_ipc[c].count = 0;
    g_ipc[c].head = 0;
    g_ipc[c].wait_recv = 0;

    int h = -1;
    for (int i = 0; i < IPC_MAX; i++)
        if (t->ipc[i] < 0) { h = i; break; }
    if (h < 0) { g_ipc[c].used = 0; return (uint64_t)(-1); }
    t->ipc[h] = c;
    return (uint64_t)h;
}

static uint64_t sys_ipc_send(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a4; (void)a5;
    int handle = (int)a1;
    const void *ubuf = (const void *)a2;
    size_t len = (size_t)a3;
    struct thread *t = sched_current();
    if (!t || handle < 0 || handle >= IPC_MAX)
        return (uint64_t)(-1);
    int c = t->ipc[handle];
    if (c < 0 || c >= IPC_CHANS || !g_ipc[c].used)
        return (uint64_t)(-1);
    struct ipc_chan *ch = &g_ipc[c];
    size_t n = len;
    if (n > IPC_MSG_LEN)
        n = IPC_MSG_LEN;
    if (ch->count >= IPC_MSG_MAX)
        return (uint64_t)(-1);
    uint8_t tmp[IPC_MSG_LEN];
    if (n && copy_from_user(tmp, ubuf, n) < 0)
        return (uint64_t)-EFAULT;
    int tail = (ch->head + ch->count) % IPC_MSG_MAX;
    struct ipc_msg *m = &ch->msgs[tail];
    __builtin_memcpy(m->data, tmp, n);
    m->len = n;
    ch->count++;
    if (ch->wait_recv)
    {
        sched_wake(ch->wait_recv);
        ch->wait_recv = 0;
    }
    return (uint64_t)n;
}

static uint64_t sys_ipc_recv(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a4; (void)a5;
    int handle = (int)a1;
    void *ubuf = (void *)a2;
    size_t max = (size_t)a3;
    struct thread *t = sched_current();
    if (!t || handle < 0 || handle >= IPC_MAX)
        return (uint64_t)(-1);
    int c = t->ipc[handle];
    if (c < 0 || c >= IPC_CHANS || !g_ipc[c].used)
        return (uint64_t)(-1);
    struct ipc_chan *ch = &g_ipc[c];
    for (;;)
    {
        if (ch->count > 0)
        {
            struct ipc_msg *m = &ch->msgs[ch->head];
            size_t n = m->len < max ? m->len : max;
            /* Copy to a kernel buffer first so a bad user pointer does not
               destroy the message. */
            uint8_t tmp[IPC_MSG_LEN];
            __builtin_memcpy(tmp, m->data, n);
            long r = copy_to_user(ubuf, tmp, n);
            if (r < 0)
                return (uint64_t)r;
            ch->head = (ch->head + 1) % IPC_MSG_MAX;
            ch->count--;
            return (uint64_t)n;
        }
        ch->wait_recv = t;
        t->state = THREAD_BLOCKED;
        sched_suspend();
    }
}

/* ---- shared memory (Phase 11) ---- */
#define SHM_MAX 16
#define SHM_BASE 0x50000000000ULL
#define SHM_SLOT 0x200000ULL

struct shm_obj {
    int id;
    int used;
    void *phys;
    uint64_t pages;
};
static struct shm_obj g_shm[SHM_MAX];
static int g_shm_next_id;

static uint64_t sys_shm_create(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a2; (void)a3; (void)a4; (void)a5;
    size_t bytes = (size_t)a1;
    if (bytes == 0)
        return (uint64_t)(-1);
    uint64_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    int s = -1;
    for (int i = 0; i < SHM_MAX; i++)
        if (!g_shm[i].used) { s = i; break; }
    if (s < 0)
        return (uint64_t)(-1);
    void *phys = pmm_alloc_frames(pages);
    if (!phys)
        return (uint64_t)(-1);
    g_shm[s].used = 1;
    g_shm[s].id = ++g_shm_next_id;
    g_shm[s].phys = phys;
    g_shm[s].pages = pages;
    return (uint64_t)g_shm[s].id;
}

static uint64_t sys_shm_attach(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a2; (void)a3; (void)a4; (void)a5;
    int id = (int)a1;
    struct thread *t = sched_current();
    if (!t)
        return (uint64_t)(-1);
    int s = -1;
    for (int i = 0; i < SHM_MAX; i++)
        if (g_shm[i].used && g_shm[i].id == id) { s = i; break; }
    if (s < 0)
        return (uint64_t)(-1);
    uint64_t va = SHM_BASE + (uint64_t)(id - 1) * SHM_SLOT;
    uint64_t phys = (uint64_t)g_shm[s].phys;
    for (uint64_t p = 0; p < g_shm[s].pages; p++)
    {
        if (vmm_map_page_in(t->mmu, va + p * PAGE_SIZE, phys + p * PAGE_SIZE,
                             MMU_USER | MMU_WRITE) < 0)
            return (uint64_t)(-1);
    }
    return va;
}

/* ---- time syscalls ---- */

/* SYS_TIME: return wall-clock seconds; optionally write to *a1. */
static uint64_t sys_time(uint64_t a1, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5)
{
    (void)a2; (void)a3; (void)a4; (void)a5;
    uint64_t sec = clock_wall_sec();
    if (a1)
    {
        if (copy_to_user((void *)a1, &sec, sizeof(sec)) < 0)
            return (uint64_t)-EFAULT;
    }
    return sec;
}

/* Kernel-side timeval layout must match userspace libc/time.h. */
struct k_timeval {
    long tv_sec;
    long tv_usec;
};

/* SYS_GETTIMEOFDAY: fill *tv with wall-clock seconds + microseconds. */
static uint64_t sys_gettimeofday(uint64_t a1, uint64_t a2, uint64_t a3,
                                 uint64_t a4, uint64_t a5)
{
    (void)a2; (void)a3; (void)a4; (void)a5;
    if (!a1)
        return 0;
    uint64_t sec, nsec;
    clock_wall_ns(&sec, &nsec);
    struct k_timeval tv;
    tv.tv_sec  = (long)sec;
    tv.tv_usec = (long)(nsec / 1000);
    return (uint64_t)copy_to_user((void *)a1, &tv, sizeof(tv));
}

/* Kernel-side timespec layout must match userspace libc/time.h. */
struct k_timespec {
    long tv_sec;
    long tv_nsec;
};

/* SYS_NANOSLEEP: block for at least req nanoseconds; write remainder to rem
   (always zero on axiomeOS — we don't support signal interruption yet). */
static uint64_t sys_nanosleep(uint64_t a1, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5)
{
    (void)a3; (void)a4; (void)a5;
    if (!a1)
        return (uint64_t)-1;
    struct k_timespec req;
    if (copy_from_user(&req, (const void *)a1, sizeof(req)) < 0)
        return (uint64_t)-EFAULT;
    if (req.tv_sec < 0 || req.tv_nsec < 0 || req.tv_nsec >= 1000000000L)
        return (uint64_t)-1;
    uint64_t ns = (uint64_t)req.tv_sec * 1000000000ULL
                + (uint64_t)req.tv_nsec;
    sched_sleep_ns(ns);
    if (a2)
    {
        struct k_timespec rem;
        rem.tv_sec  = 0;
        rem.tv_nsec = 0;
        if (copy_to_user((void *)a2, &rem, sizeof(rem)) < 0)
            return (uint64_t)-EFAULT;
    }
    return 0;
}

/* ---- file positioning / positional I/O (issue #28) ---- */

static uint64_t sys_lseek(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a4; (void)a5;
    int fd = (int)a1;
    long offset = (long)a2;
    int whence = (int)a3;
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END)
        return (uint64_t)-EINVAL;
    vfs_ensure_proc();
    struct thread *t = sched_current();
    if (!t || fd < 0 || fd >= MAX_FD || !t->fds[fd].used)
        return (uint64_t)-EBADF;
    struct vfs_file *f = &t->fds[fd];
    if (f->kind == FD_VNODE)
    {
        long base = 0;
        if (whence == SEEK_SET)
            base = 0;
        else if (whence == SEEK_CUR)
            base = (long)f->off;
        else if (whence == SEEK_END)
            base = (long)f->node->size;
        long pos = base + offset;
        if (pos < 0)
            return (uint64_t)-EINVAL;
        f->off = (size_t)pos;
        return (uint64_t)f->off;
    }
    return (uint64_t)-ESPIPE;
}

static uint64_t sys_pread(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a5;
    int fd = (int)a1;
    char *buf = (char *)a2;
    size_t len = (size_t)a3;
    long off = (long)a4;
    if (off < 0)
        return (uint64_t)-EINVAL;
    if (len && !user_range_ok((uint64_t)buf, len, 1))
        return (uint64_t)-EFAULT;
    vfs_ensure_proc();
    struct thread *t = sched_current();
    if (!t || fd < 0 || fd >= MAX_FD || !t->fds[fd].used)
        return (uint64_t)-EBADF;
    struct vfs_file *f = &t->fds[fd];
    if (f->kind != FD_VNODE || !f->node)
        return (uint64_t)-ESPIPE;
    if (vfs_check_perms(f->node, VFS_MAY_READ) != 0)
        return (uint64_t)-EACCES;
    return (uint64_t)vfs_read(f->node, (size_t)off, buf, len);
}

static uint64_t sys_pwrite(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a5;
    int fd = (int)a1;
    const char *s = (const char *)a2;
    size_t len = (size_t)a3;
    long off = (long)a4;
    if (off < 0)
        return (uint64_t)-EINVAL;
    if (len && !user_range_ok((uint64_t)s, len, 0))
        return (uint64_t)-EFAULT;
    vfs_ensure_proc();
    struct thread *t = sched_current();
    if (!t || fd < 0 || fd >= MAX_FD || !t->fds[fd].used)
        return (uint64_t)-EBADF;
    struct vfs_file *f = &t->fds[fd];
    if (f->kind != FD_VNODE || !f->node)
        return (uint64_t)-ESPIPE;
    long w = vfs_write(f->node, (size_t)off, s, len);
    if (w < 0)
        return (uint64_t)(size_t)w;
    return (uint64_t)w;
}

static syscall_fn syscall_table[] = {
    [SYS_PRINT]  = sys_print,
    [SYS_YIELD]  = sys_yield,
    [SYS_EXIT]   = sys_exit,
    [SYS_FORK]   = sys_fork,
    [SYS_GETPID] = sys_getpid,
    [SYS_WAITPID] = sys_waitpid,
    [SYS_WRITE]  = sys_write,
    [SYS_READ]   = sys_read,
    [SYS_SPAWN_CMD] = sys_spawn_cmd,
    [SYS_PS]     = sys_ps,
    [SYS_OPEN]    = sys_open,
    [SYS_CLOSE]   = sys_close,
    [SYS_MKDIR]   = sys_mkdir,
    [SYS_UNLINK]  = sys_unlink,
    [SYS_READDIR] = sys_readdir,
    [SYS_CHDIR]   = sys_chdir,
    [SYS_GETCWD]  = sys_getcwd,
    [SYS_FSTAT]   = sys_fstat,
    [SYS_DUP2]    = sys_dup2,
    [SYS_PIPE]    = sys_pipe,
    [SYS_MOUNT]   = sys_mount,
    [SYS_UMOUNT]  = sys_umount,
    [SYS_KILL]    = sys_kill,
    [SYS_SIGACTION] = sys_sigaction,
    [SYS_SIGRETURN] = sys_sigreturn,
    [SYS_IPC_CREATE] = sys_ipc_create,
    [SYS_IPC_SEND] = sys_ipc_send,
    [SYS_IPC_RECV] = sys_ipc_recv,
    [SYS_SHM_CREATE] = sys_shm_create,
    [SYS_SHM_ATTACH] = sys_shm_attach,
    [SYS_MKFIFO]  = sys_mkfifo,
    [SYS_DRIVER_RESCAN] = sys_drv_rescan,
    [SYS_SOCKET_CREATE] = sys_socket_create,
    [SYS_SOCKET_BIND]   = sys_socket_bind,
    [SYS_SOCKET_CONNECT] = sys_socket_connect,
    [SYS_SOCKET_SEND]   = sys_socket_send,
    [SYS_SOCKET_RECV]   = sys_socket_recv,
    [SYS_SOCKET_CLOSE]  = sys_socket_close,
    [SYS_SOCKET_LISTEN] = sys_socket_listen,
    [SYS_SOCKET_ACCEPT] = sys_socket_accept,
    /* DNS / ICMP services (Phase 13) */
    [SYS_DNS_RESOLVE]   = sys_dns_resolve,
    [SYS_PING]          = sys_ping,
    [SYS_GETUID]  = sys_getuid,
    [SYS_GETEUID] = sys_geteuid,
    [SYS_GETGID]  = sys_getgid,
    [SYS_GETEGID] = sys_getegid,
    [SYS_SETUID]  = sys_setuid,
    [SYS_SETGID]  = sys_setgid,
    [SYS_GETROLE] = sys_getrole,
    [SYS_CHMOD]   = sys_chmod,
    [SYS_CHOWN]   = sys_chown,
    [SYS_GETCAP]  = sys_getcap,
    [SYS_SETCAP]  = sys_setcap,
    [SYS_GETPWNAM] = sys_getpwnam,
    /* loadable kernel modules (.kxt) */
    [SYS_MODULE_LOAD]   = sys_module_load,
    [SYS_MODULE_UNLOAD] = sys_module_unload,
    [SYS_MMAP]          = sys_mmap,
    [SYS_UNAME]         = sys_uname,
    [SYS_RELOAD_USERS]  = sys_reload_users,
    [SYS_EXECVE]        = sys_execve,
    /* time */
    [SYS_TIME]          = sys_time,
    [SYS_GETTIMEOFDAY]  = sys_gettimeofday,
    [SYS_NANOSLEEP]     = sys_nanosleep,
    /* file positioning / positional I/O (issue #28) */
    [SYS_LSEEK]         = sys_lseek,
    [SYS_PREAD]         = sys_pread,
    [SYS_PWRITE]        = sys_pwrite,
    /* path-based stat (issue #28) */
    [SYS_STAT]          = sys_stat,
    [SYS_RMDIR]         = sys_rmdir,
    [SYS_AUTHENTICATE]  = sys_authenticate,
    /* non-blocking reap for compositors */
    [SYS_WAITPID_NB]    = sys_waitpid_nb,
};

/* ABI guard (issue #33): the table must cover every number defined in the
   shared syscall_numbers.h, otherwise a handler is missing for a syscall the
   userspace libc already exposes. */
_Static_assert(sizeof(syscall_table) / sizeof(syscall_fn) >= __SYS_LAST,
               "syscall table does not cover all syscall numbers");

static int syscall_count = sizeof(syscall_table) / sizeof(syscall_fn);

uint64_t syscall_dispatch(uint64_t n, uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5)
{
    if (n >= (uint64_t)syscall_count || !syscall_table[n])
    {
        printk("SYS: unknown syscall %lu\n", n);
        return -1;
    }
    uint64_t ret = syscall_table[n](a1, a2, a3, a4, a5);
    syscall_deliver_signals();
    return ret;
}

void syscall_init(void)
{
    current_kstack_top = (uint64_t)syscall_stack_page + sizeof(syscall_stack_page);

    tss_set_rsp0(current_kstack_top);

    uint64_t star = 0;
    star |= (uint64_t)0x18 << 32;
    star |= (uint64_t)0x10 << 48;

    uint64_t lstar = (uint64_t)syscall_entry;
    uint64_t sfmask = 0x200;

    __asm__ volatile("wrmsr" : : "a"((uint32_t)star), "d"((uint32_t)(star >> 32)), "c"(0xC0000081));
    __asm__ volatile("wrmsr" : : "a"((uint32_t)lstar), "d"((uint32_t)(lstar >> 32)), "c"(0xC0000082));
    __asm__ volatile("wrmsr" : : "a"((uint32_t)sfmask), "d"((uint32_t)(sfmask >> 32)), "c"(0xC0000084));

    printk("Syscall: MSRs configured (entry=0x%lx)\n", lstar);
}
