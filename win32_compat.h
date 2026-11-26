/*
 * win32_compat.h: the few POSIX functions that ntrun uses and that a native Windows C
 * library (MSYS2 UCRT64, mingw-w64) does not have. Include it after the system headers.
 * What cannot be a small shim (mmap of the guest memory, fork, the SIGSEGV handler) is in
 * ntrun.c and cmd.c under #ifdef _WIN32.
 */
#ifndef WIN32_COMPAT_H
#define WIN32_COMPAT_H

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <direct.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <limits.h>
#include <process.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* mingw's limits.h has 260 for PATH_MAX, too small for the host paths of the guest */
#undef PATH_MAX
#define PATH_MAX 4096
#ifndef NAME_MAX
#define NAME_MAX 255
#endif

/* sigsetjmp and siglongjmp: there is no signal mask to save */
#define sigjmp_buf jmp_buf
#define sigsetjmp(env, savemask) setjmp(env)
#define siglongjmp(env, val) longjmp(env, val)

/* file types and modes */
#ifndef S_ISSOCK
#define S_ISSOCK(m) 0
#endif
#define lstat(path, buf) stat((path), (buf))
#define mkdir(path, mode) _mkdir(path)

/* fcntl: a file descriptor is valid or not, the other commands do nothing */
#define F_GETFD 1
#define F_SETFD 2
#define F_SETFL 4
#define FD_CLOEXEC 1
#define O_NONBLOCK 0x4000
static inline int fcntl(int fd, int cmd, ...)
{
    if (cmd == F_GETFD)
        return _get_osfhandle(fd) == -1 ? -1 : 0;
    return 0;
}

/* host paths have / between names, also those that the host gives */
static inline char *nt_getcwd(char *buf, size_t n)
{
    if (!_getcwd(buf, (int)n))
        return NULL;
    for (char *s = buf; *s; s++)
        if (*s == '\\')
            *s = '/';
    return buf;
}
#define getcwd nt_getcwd

static inline char *nt_realpath(const char *path, char *resolved)
{
    char tmp[PATH_MAX];

    if (!_fullpath(tmp, path, sizeof tmp))
        return NULL;
    if (_access(tmp, 0) < 0)
        return NULL;
    for (char *s = tmp; *s; s++)
        if (*s == '\\')
            *s = '/';
    if (!resolved)
        return _strdup(tmp);
    snprintf(resolved, PATH_MAX, "%s", tmp);
    return resolved;
}
#define realpath nt_realpath

/* a pipe of file descriptors: both ends binary, and inherited by the programs that we start (the
   guest hands a pipe to a child process, as _popen does; a Unix pipe is inherited too) */
static inline int nt_pipe(int fds[2])
{
    return _pipe(fds, 65536, _O_BINARY);
}
#define pipe nt_pipe

/* pread and pwrite: the position of the file stays where it was */
static inline ssize_t nt_pread(int fd, void *buf, size_t count, long long off)
{
    long long old = _lseeki64(fd, 0, SEEK_CUR);
    int r;

    if (old < 0 || _lseeki64(fd, off, SEEK_SET) < 0)
        return -1;
    r = _read(fd, buf, (unsigned)count);
    _lseeki64(fd, old, SEEK_SET);
    return r;
}
static inline ssize_t nt_pwrite(int fd, const void *buf, size_t count, long long off)
{
    long long old = _lseeki64(fd, 0, SEEK_CUR);
    int r;

    if (old < 0 || _lseeki64(fd, off, SEEK_SET) < 0)
        return -1;
    r = _write(fd, buf, (unsigned)count);
    _lseeki64(fd, old, SEEK_SET);
    return r;
}
#define pread nt_pread
#define pwrite nt_pwrite

/* time */
#ifndef localtime_r
static inline struct tm *nt_localtime_r(const time_t *t, struct tm *r) { return localtime_s(r, t) == 0 ? r : NULL; }
#define localtime_r nt_localtime_r
#endif
#ifndef gmtime_r
static inline struct tm *nt_gmtime_r(const time_t *t, struct tm *r) { return gmtime_s(r, t) == 0 ? r : NULL; }
#define gmtime_r nt_gmtime_r
#endif
#define timegm _mkgmtime
/* seconds east of UTC for a local time (tm_gmtoff of the Unix libraries): the zone, and the
   summer time bias when tm_isdst is set */
static inline long nt_gmtoff(const struct tm *tm)
{
    long tz = 0, db = 0;

    _tzset();
    _get_timezone(&tz);
    if (tm->tm_isdst > 0)
        _get_dstbias(&db);
    return -tz - db;
}

static inline int nt_utimes(const char *path, const struct timeval tv[2])
{
    HANDLE h = CreateFileA(path, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    FILETIME fa, fw;
    ULARGE_INTEGER u;
    int ok;

    if (h == INVALID_HANDLE_VALUE)
        return -1;
    u.QuadPart = ((unsigned long long)tv[0].tv_sec + 11644473600ULL) * 10000000ULL + (unsigned long long)tv[0].tv_usec * 10;
    fa.dwLowDateTime = u.LowPart;
    fa.dwHighDateTime = u.HighPart;
    u.QuadPart = ((unsigned long long)tv[1].tv_sec + 11644473600ULL) * 10000000ULL + (unsigned long long)tv[1].tv_usec * 10;
    fw.dwLowDateTime = u.LowPart;
    fw.dwHighDateTime = u.HighPart;
    ok = SetFileTime(h, NULL, &fa, &fw);
    CloseHandle(h);
    return ok ? 0 : -1;
}
static inline int nt_futimes(int fd, const struct timeval tv[2])
{
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    FILETIME fa, fw;
    ULARGE_INTEGER u;

    u.QuadPart = ((unsigned long long)tv[0].tv_sec + 11644473600ULL) * 10000000ULL + (unsigned long long)tv[0].tv_usec * 10;
    fa.dwLowDateTime = u.LowPart;
    fa.dwHighDateTime = u.HighPart;
    u.QuadPart = ((unsigned long long)tv[1].tv_sec + 11644473600ULL) * 10000000ULL + (unsigned long long)tv[1].tv_usec * 10;
    fw.dwLowDateTime = u.LowPart;
    fw.dwHighDateTime = u.HighPart;
    return SetFileTime(h, NULL, &fa, &fw) ? 0 : -1;
}
#define utimes nt_utimes
#define futimes nt_futimes

/* environment and random numbers */
static inline int nt_setenv(const char *name, const char *value, int overwrite)
{
    if (!overwrite && getenv(name))
        return 0;
    return _putenv_s(name, value ? value : "");
}
static inline int nt_unsetenv(const char *name) { return _putenv_s(name, ""); }
#define setenv nt_setenv
#define unsetenv nt_unsetenv

/* ioctl(fd, FIONREAD, &n): the bytes that a pipe has ready */
#define FIONREAD 0x541B
static inline int nt_ioctl(int fd, unsigned long req, int *avail)
{
    DWORD bytes = 0;
    HANDLE h = (HANDLE)_get_osfhandle(fd);

    *avail = 0;
    if (req != FIONREAD)
        return -1;
    if (GetFileType(h) == FILE_TYPE_PIPE) {
        if (!PeekNamedPipe(h, NULL, 0, NULL, &bytes, NULL))
            return -1;
    } else {
        long long cur = _lseeki64(fd, 0, SEEK_CUR), end = cur < 0 ? -1 : _lseeki64(fd, 0, SEEK_END);
        if (end < 0)
            return -1;
        _lseeki64(fd, cur, SEEK_SET);
        bytes = (DWORD)(end - cur);
    }
    *avail = (int)bytes;
    return 0;
}
#define ioctl nt_ioctl

#endif /* _WIN32 */
#endif /* WIN32_COMPAT_H */
