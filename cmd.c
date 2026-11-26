/*
 * cmd.c: the command processor of ntrun.
 *
 * nmake and the compiler drivers give command lines to "cmd /c". There is no CMD.EXE
 * here, so ntrun does the work itself: the things that the makefiles and the batch
 * files of the SDKs use.
 *
 *   operators   &  &&  ||  |  ( )  >  >>  <  2>  2>&1  ^
 *   variables   %NAME%  %ERRORLEVEL%  %CD%  the variable of FOR;
 *               in a batch file also %0 to %9, %* and %%
 *   commands    call cd chdir copy del dir echo endlocal erase exit for goto if md mkdir
 *               move path pause popd pushd rd rem ren rename rmdir set setlocal shift
 *               type ver (and cls, title, break, verify, prompt, color: they do nothing)
 *   programs    .exe and .com files run in a new ntrun; .bat and .cmd files run here
 *
 * Not done: SET /A and /P, FOR /F /L /R /D, START, lines that wrap to a width.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#ifndef _WIN32
#include <sys/wait.h>
#endif
#include <time.h>
#include <unistd.h>
#include "win32_compat.h"

#include "ntrun.h"

#ifdef _WIN32
/* win32_compat.h gives 4096: the limits.h of mingw has 260, too small for the host paths */
_Static_assert(PATH_MAX >= 4096, "PATH_MAX is too small for host paths");
#endif

static u32 errorlevel;
static int echo_on = 1;
static int abort_line; /* a GOTO or the end of the batch file: leave the rest of the line */

struct batch {
    struct batch *up;
    char **line;
    int n, pc, done;
    char *arg[64];
    int narg, shift;
};
static struct batch *bat; /* the batch file that runs now */

struct local { /* SETLOCAL: the environment and the folder to go back to */
    struct local *up;
    char **env;
    int n;
    char cwd[PATH_MAX];
};
static struct local *locals;
static int nlocals;

static char *dirstack[32]; /* PUSHD */
static int ndirstack;

static u32 run_seq(char *s);
static u32 run_command(char *s);

int cmd_is_name(const char *prog)
{
    const char *b = strrchr(prog, '\\');
    const char *b2 = strrchr(prog, '/');

    if (b2 && (!b || b2 > b))
        b = b2;
    b = b ? b + 1 : prog;
    return !strcasecmp(b, "cmd.exe") || !strcasecmp(b, "cmd") || !strcasecmp(b, "command.com") ||
        !strcasecmp(b, "command");
}

/* ---------------- text ---------------- */

static char *skip_ws(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

/* is the word W at the start of S? (the letters in any case, then a character that ends a word) */
static int is_word(const char *s, const char *w)
{
    size_t n = strlen(w);
    return !strncasecmp(s, w, n) && (!s[n] || strchr(" \t(/", s[n]));
}

/* the next word of *S, with the quotes taken off; NULL at the end. It changes the text. */
static char *next_word(char **s)
{
    char *p = skip_ws(*s), *w, *o;

    if (!*p)
        return NULL;
    w = o = p;
    while (*p && *p != ' ' && *p != '\t') {
        if (*p == '"') {
            for (p++; *p && *p != '"'; p++)
                *o++ = *p;
            if (*p)
                p++;
        } else {
            *o++ = *p++;
        }
    }
    if (*p)
        p++;
    *o = 0;
    *s = p;
    return w;
}

/* the ) that closes the ( at S; NULL if there is none */
static char *find_close(char *s)
{
    int q = 0, depth = 0;

    for (char *p = s; *p; p++) {
        if (*p == '"') {
            q = !q;
        } else if (!q && *p == '^' && p[1]) {
            p++;
        } else if (!q && *p == '(') {
            depth++;
        } else if (!q && *p == ')' && --depth == 0) {
            return p;
        }
    }
    return NULL;
}

/* how many ( of S have no ) */
static int paren_depth(const char *s)
{
    int q = 0, depth = 0;

    for (; *s; s++) {
        if (*s == '"')
            q = !q;
        else if (!q && *s == '^' && s[1])
            s++;
        else if (!q && *s == '(')
            depth++;
        else if (!q && *s == ')' && depth)
            depth--;
    }
    return depth;
}

/* The first operator of S that is not in quotes and not in ( ):
   with PIPE 0: & && ||; with PIPE 1: a single |. NULL if there is none. */
static char *find_op(char *s, int pipe)
{
    int q = 0, depth = 0;

    for (char *p = s; *p; p++) {
        if (*p == '"') {
            q = !q;
        } else if (q) {
            ;
        } else if (*p == '^' && p[1]) {
            p++;
        } else if (*p == '(') {
            depth++;
        } else if (*p == ')') {
            if (depth)
                depth--;
        } else if (depth) {
            ;
        } else if (*p == '&') {
            if (p > s && (p[-1] == '>' || p[-1] == '<'))
                continue; /* 2>&1 */
            if (!pipe)
                return p;
        } else if (*p == '|') {
            if (p[1] == '|') {
                if (!pipe)
                    return p;
                p++;
            } else if (pipe) {
                return p;
            }
        }
    }
    return NULL;
}

/* a text that grows */
struct text {
    char *s;
    size_t n, cap;
};

static void text_add(struct text *t, const char *s, size_t len)
{
    if (t->n + len + 1 > t->cap) {
        t->cap = (t->n + len + 1) * 2 + 64;
        t->s = realloc(t->s, t->cap);
    }
    memcpy(t->s + t->n, s, len);
    t->n += len;
    t->s[t->n] = 0;
}

static void guest_cwd(char *out, size_t n)
{
    to_guest(".", out, n);
    n = strlen(out);
    if (n > 4 && out[n - 1] == '.' && out[n - 2] == '\\')
        out[n - 2] = 0; /* "Z:\folder\." gives "Z:\folder" */
    else if (n > 1 && out[n - 1] == '.')
        out[n - 1] = 0; /* "Z:\." gives "Z:\" */
}

/* the value of a variable; NULL if it has none */
static const char *var_value(const char *name, char *tmp, size_t n)
{
    const char *v = env_get(name);

    if (v)
        return v;
    if (!strcasecmp(name, "ERRORLEVEL")) {
        snprintf(tmp, n, "%d", (int)errorlevel);
        return tmp;
    }
    if (!strcasecmp(name, "CD")) {
        guest_cwd(tmp, n);
        return tmp;
    }
    return NULL;
}

/* The % signs of a line: %NAME% gives the value of a variable. In a batch file %0 to %9 and
   %* are its arguments, %% is one %, and a name with no value gives nothing. The result is new. */
static char *expand(const char *s)
{
    struct text t = { NULL, 0, 0 };
    char name[256], tmp[PATH_MAX];

    text_add(&t, "", 0);
    while (*s) {
        const char *e;
        if (*s != '%') {
            text_add(&t, s++, 1);
            continue;
        }
        if (bat && s[1] == '%') {
            text_add(&t, "%", 1);
            s += 2;
            continue;
        }
        if (bat && isdigit((unsigned char)s[1])) {
            int k = s[1] - '0' + bat->shift;
            if (k < bat->narg)
                text_add(&t, bat->arg[k], strlen(bat->arg[k]));
            s += 2;
            continue;
        }
        if (bat && s[1] == '*') {
            for (int k = 1 + bat->shift; k < bat->narg; k++) {
                if (k > 1 + bat->shift)
                    text_add(&t, " ", 1);
                text_add(&t, bat->arg[k], strlen(bat->arg[k]));
            }
            s += 2;
            continue;
        }
        e = strchr(s + 1, '%');
        if (e && e > s + 1 && (size_t)(e - s - 1) < sizeof name) {
            const char *v;
            memcpy(name, s + 1, (size_t)(e - s - 1));
            name[e - s - 1] = 0;
            v = var_value(name, tmp, sizeof tmp);
            if (v) {
                text_add(&t, v, strlen(v));
                s = e + 1;
                continue;
            }
            if (bat && !strpbrk(name, " \t")) {
                s = e + 1;
                continue;
            }
        }
        text_add(&t, s++, 1);
    }
    return t.s;
}

/* take the ^ signs out: ^ makes the next character an ordinary one */
static void uncaret(char *s)
{
    char *o = s;
    int q = 0;

    for (; *s; s++) {
        if (*s == '"')
            q = !q;
        if (!q && *s == '^' && s[1])
            s++;
        *o++ = *s;
    }
    *o = 0;
}

/* ---------------- redirection ---------------- */

struct redir {
    int n, fd[8], saved[8];
};

static void redir_set(struct redir *r, int fd, int to)
{
    if (r->n == 8)
        return;
    fflush(stdout);
    fflush(stderr);
    r->fd[r->n] = fd;
    r->saved[r->n] = dup(fd);
    r->n++;
    dup2(to, fd);
}

static void redir_undo(struct redir *r)
{
    fflush(stdout);
    fflush(stderr);
    while (r->n) {
        r->n--;
        if (r->saved[r->n] >= 0) {
            dup2(r->saved[r->n], r->fd[r->n]);
            close(r->saved[r->n]);
        } else {
            close(r->fd[r->n]);
        }
    }
}

/* Take the redirections (>file >>file <file 2>file 2>&1) out of S and do them.
   The result is 0 when a file cannot be opened. */
static int redirect(char *s, struct redir *r)
{
    int q = 0;
    char *p, *o;

    for (p = o = s; *p;) {
        int fdn = -1, app = 0, in, fd;
        char name[PATH_MAX], hp[PATH_MAX];
        size_t n = 0;
        char *start = p;

        if (*p == '"') {
            q = !q;
            *o++ = *p++;
            continue;
        }
        if (q) {
            *o++ = *p++;
            continue;
        }
        if (*p == '^' && p[1]) {
            *o++ = *p++;
            *o++ = *p++;
            continue;
        }
        if (isdigit((unsigned char)*p) && (p[1] == '>' || p[1] == '<') && (p == s || p[-1] == ' ' || p[-1] == '\t')) {
            fdn = *p - '0';
            p++;
        }
        if (*p != '>' && *p != '<') {
            p = start;
            *o++ = *p++;
            continue;
        }
        in = *p == '<';
        if (fdn < 0)
            fdn = in ? 0 : 1;
        p++;
        if (*p == '>') {
            app = 1;
            p++;
        }
        if (*p == '&' && isdigit((unsigned char)p[1])) { /* 2>&1 */
            redir_set(r, fdn, p[1] - '0');
            p += 2;
            continue;
        }
        p = skip_ws(p);
        if (*p == '"') {
            for (p++; *p && *p != '"' && n < sizeof name - 1; p++)
                name[n++] = *p;
            if (*p)
                p++;
        } else {
            while (*p && *p != ' ' && *p != '\t' && *p != '>' && *p != '<' && n < sizeof name - 1)
                name[n++] = *p++;
        }
        name[n] = 0;
        to_host(name, hp, sizeof hp);
        fd = in ? open(hp, O_RDONLY) : open(hp, O_WRONLY | O_CREAT | (app ? O_APPEND : O_TRUNC), 0666);
        if (fd < 0) {
            fprintf(stderr, in ? "The system cannot find the file specified.\r\n" : "The system cannot find the path specified.\r\n");
            *o = 0;
            return 0;
        }
        redir_set(r, fdn, fd);
        close(fd);
    }
    *o = 0;
    return 1;
}

/* ---------------- files ---------------- */

struct flist {
    char **name; /* host paths */
    int n;
};

static void flist_add(struct flist *f, const char *hp)
{
    f->name = realloc(f->name, sizeof *f->name * (size_t)(f->n + 1));
    f->name[f->n++] = strdup(hp);
}

static void flist_free(struct flist *f)
{
    for (int i = 0; i < f->n; i++)
        free(f->name[i]);
    free(f->name);
    f->name = NULL;
    f->n = 0;
}

static int has_wild(const char *s) { return strpbrk(s, "*?") != NULL; }

static int cmp_names(const void *a, const void *b) { return strcasecmp(*(char *const *)a, *(char *const *)b); }

/* The files that the Windows name or pattern G is for. DIRS: also folders. With a pattern
   the list is in the order of the names. */
static void files_of(const char *g, struct flist *f, int dirs)
{
    char tmp[PATH_MAX], hdir[PATH_MAX], hp[PATH_MAX * 2], *bs, *pat;
    struct stat st;
    DIR *d;
    struct dirent *e;
    int first = f->n;

    snprintf(tmp, sizeof tmp, "%s", g);
    for (char *s = tmp; *s; s++)
        if (*s == '/')
            *s = '\\';
    if (!has_wild(tmp)) {
        to_host(tmp, hp, PATH_MAX);
        if (stat(hp, &st) == 0 && (dirs || !S_ISDIR(st.st_mode)))
            flist_add(f, hp);
        return;
    }
    bs = strrchr(tmp, '\\');
    if (bs) {
        *bs = 0;
        pat = bs + 1;
        to_host(tmp[0] && !(tmp[1] == ':' && !tmp[2]) ? tmp : "\\", hdir, sizeof hdir);
    } else if (tmp[0] && tmp[1] == ':') {
        pat = tmp + 2;
        snprintf(hdir, sizeof hdir, ".");
    } else {
        pat = tmp;
        snprintf(hdir, sizeof hdir, ".");
    }
    if (!(d = opendir(hdir)))
        return;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || !wild_name(pat, e->d_name))
            continue;
        if (!strcmp(hdir, "."))
            snprintf(hp, sizeof hp, "%s", e->d_name);
        else
            snprintf(hp, sizeof hp, "%s/%s", strcmp(hdir, "/") ? hdir : "", e->d_name);
        if (stat(hp, &st) == 0 && (dirs || !S_ISDIR(st.st_mode)))
            flist_add(f, hp);
    }
    closedir(d);
    qsort(f->name + first, (size_t)(f->n - first), sizeof *f->name, cmp_names);
}

static const char *base_of(const char *hp)
{
    const char *b = strrchr(hp, '/');
    return b ? b + 1 : hp;
}

static int is_dir(const char *hp)
{
    struct stat st;
    return stat(hp, &st) == 0 && S_ISDIR(st.st_mode);
}

static int same_file(const char *a, const char *b)
{
#ifdef _WIN32
    /* the host gives no inode numbers: the same full path is the same file */
    char pa[PATH_MAX], pb[PATH_MAX];
    return realpath(a, pa) && realpath(b, pb) && !strcasecmp(pa, pb);
#else
    struct stat sa, sb;
    return stat(a, &sa) == 0 && stat(b, &sb) == 0 && sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
#endif
}

/* Copy the data of the file FROM to the open file OUT; 0 if it cannot be read.
   TEXT: the data ends at the first Ctrl-Z, the end mark of a text file. */
static int copy_data(const char *from, int out, int text)
{
    char buf[65536], *z;
    ssize_t n;
    int in = open(from, O_RDONLY), ok = 1;

    if (in < 0)
        return 0;
    while ((n = read(in, buf, sizeof buf)) > 0) {
        z = text ? memchr(buf, 0x1a, (size_t)n) : NULL;
        if (z)
            n = z - buf;
        if (write(out, buf, (size_t)n) != n) {
            ok = 0;
            break;
        }
        if (z) {
            n = 0;
            break;
        }
    }
    close(in);
    return ok && n == 0;
}

/* copy one file; the copy keeps the time of the file, as on Windows */
static int copy_file(const char *from, const char *to)
{
    struct stat st;
    struct timeval tv[2];
    int out, ok;

    if (stat(from, &st) < 0 || S_ISDIR(st.st_mode))
        return 0;
    if (same_file(from, to)) {
        fprintf(stderr, "The file cannot be copied onto itself.\r\n");
        return 0;
    }
    out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out < 0)
        return 0;
    ok = copy_data(from, out, 0);
    close(out);
    tv[0].tv_sec = st.st_atime;
    tv[0].tv_usec = 0;
    tv[1].tv_sec = st.st_mtime;
    tv[1].tv_usec = 0;
    utimes(to, tv);
    return ok;
}

/* ---------------- the commands ---------------- */

static u32 fail(const char *msg)
{
    fprintf(stderr, "%s\r\n", msg);
    return 1;
}

static u32 do_echo(char *args)
{
    char *t;

    if (*args == '.' || *args == ':' || *args == '/' || *args == ',' || *args == ';' || *args == '(') {
        printf("%s\r\n", args + 1); /* "echo." is an empty line */
        return 0;
    }
    t = skip_ws(args);
    if (!*t) {
        printf("ECHO is %s.\r\n", echo_on ? "on" : "off");
        return 0;
    }
    if (!strcasecmp(t, "on") || !strncasecmp(t, "on ", 3)) {
        echo_on = 1;
    } else if (!strcasecmp(t, "off") || !strncasecmp(t, "off ", 4)) {
        echo_on = 0;
    } else {
        uncaret(args);
        printf("%s\r\n", *args == ' ' || *args == '\t' ? args + 1 : args);
    }
    return 0;
}

static u32 do_cd(char *args)
{
    char hp[PATH_MAX], g[PATH_MAX], *w;

    do
        w = next_word(&args);
    while (w && !strcasecmp(w, "/d"));
    if (!w || (isalpha((unsigned char)w[0]) && w[1] == ':' && !w[2])) {
        guest_cwd(g, sizeof g);
        printf("%s\r\n", g);
        return 0;
    }
    to_host(w, hp, sizeof hp);
    if (chdir(hp) < 0)
        return fail("The system cannot find the path specified.");
    return 0;
}

static u32 do_md(char *args)
{
    char hp[PATH_MAX], *w;
    u32 st = 0;

    while ((w = next_word(&args))) {
        to_host(w, hp, sizeof hp);
        if (is_dir(hp)) {
            fprintf(stderr, "A subdirectory or file %s already exists.\r\n", w);
            st = 1;
            continue;
        }
        for (char *s = hp + 1; *s; s++) /* the folders above it too */
            if (*s == '/') {
                *s = 0;
                mkdir(hp, 0777);
                *s = '/';
            }
        if (mkdir(hp, 0777) < 0)
            st = fail("The system cannot find the path specified.");
    }
    return st;
}

/* remove a folder and all that is in it */
static int remove_tree(const char *hp)
{
    char sub[PATH_MAX * 2];
    struct stat st;
    DIR *d;
    struct dirent *e;

    if (lstat(hp, &st) < 0)
        return 0;
    if (!S_ISDIR(st.st_mode))
        return unlink(hp) == 0;
    if (!(d = opendir(hp)))
        return 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(sub, sizeof sub, "%s/%s", hp, e->d_name);
        remove_tree(sub);
    }
    closedir(d);
    return rmdir(hp) == 0;
}

static u32 do_rd(char *args)
{
    char hp[PATH_MAX], real[PATH_MAX], cwd[PATH_MAX], *w;
    int tree = 0;
    u32 st = 0;

    while ((w = next_word(&args))) {
        if (w[0] == '/') {
            if (strstr(w, "/s") || strstr(w, "/S"))
                tree = 1;
            continue;
        }
        to_host(w, hp, sizeof hp);
        if (!tree) {
            if (rmdir(hp) < 0)
                st = fail(errno == ENOTEMPTY || errno == EEXIST ? "The directory is not empty." :
                    "The system cannot find the file specified.");
            continue;
        }
        /* RD /S takes all away. Every drive is the root of the host here, so a wrong makefile line
           could take much: only a folder below the current folder is removed. */
        if (!realpath(hp, real) || !getcwd(cwd, sizeof cwd) || strncmp(real, cwd, strlen(cwd)) ||
            real[strlen(cwd)] != '/' || strlen(cwd) < 2) {
            fprintf(stderr, "ntrun: RD /S removes only a folder below the current folder: %s\r\n", w);
            st = 1;
            continue;
        }
        if (!remove_tree(real))
            st = 1;
    }
    return st;
}

static u32 do_del(char *args)
{
    char *w;
    u32 st = 0;

    while ((w = next_word(&args))) {
        struct flist f = { NULL, 0 };
        char pat[PATH_MAX], hp[PATH_MAX];
        if (w[0] == '/' && !strchr(w + 1, '\\') && strlen(w) <= 3)
            continue; /* a switch */
        snprintf(pat, sizeof pat, "%s", w);
        to_host(w, hp, sizeof hp);
        if (!has_wild(w) && is_dir(hp))
            snprintf(pat, sizeof pat, "%s\\*", w); /* a folder: all the files in it */
        files_of(pat, &f, 0);
        if (!f.n)
            fprintf(stderr, "Could Not Find %s\r\n", w);
        for (int i = 0; i < f.n; i++)
            if (unlink(f.name[i]) < 0)
                st = fail("Access is denied.");
        flist_free(&f);
    }
    return st;
}

static u32 do_copy(char *args)
{
    char *words[64], *src, *dst = NULL, hdst[PATH_MAX], to[PATH_MAX * 2];
    int nw = 0, i, copied = 0, text = 1;
    struct text spec = { NULL, 0, 0 };
    struct flist f = { NULL, 0 };
    char *w;
    u32 st = 0;

    while ((w = next_word(&args)) && nw < 64) {
        if (w[0] == '/' && strlen(w) <= 3) { /* /b: the files are not text; /a /v /y /-y: nothing to do */
            if (!strcasecmp(w, "/b"))
                text = 0;
        } else {
            words[nw++] = w;
        }
    }
    if (!nw)
        return fail("The syntax of the command is incorrect.");
    /* the first words, joined by +, are what to copy; the word after them is where to */
    if (!words[0][0])
        return fail("The syntax of the command is incorrect.");
    text_add(&spec, words[0], strlen(words[0]));
    for (i = 1; i < nw; i++) {
        if (words[i][0] == '+' || spec.s[spec.n - 1] == '+')
            text_add(&spec, words[i], strlen(words[i]));
        else
            break;
    }
    src = spec.s;
    if (i < nw)
        dst = words[i];
    if (dst)
        to_host(dst, hdst, sizeof hdst);
    else
        snprintf(hdst, sizeof hdst, ".");

    if (strchr(src, '+')) {
        /* a+b+c: the files one after the other in one file */
        char *part, *save = NULL, first[PATH_MAX];
        int out, append = 0;
        part = strtok_r(src, "+", &save);
        to_host(part ? part : "", first, sizeof first);
        if (!dst)
            snprintf(hdst, sizeof hdst, "%s", first);
        if (is_dir(hdst)) {
            size_t l = strlen(hdst);
            snprintf(hdst + l, sizeof hdst - l, "/%s", base_of(first));
        }
        append = same_file(first, hdst); /* "copy a+b a": b goes to the end of a */
        out = open(hdst, O_RDWR | O_CREAT | (append ? 0 : O_TRUNC), 0666);
        if (out < 0) {
            free(spec.s);
            return fail("The system cannot find the path specified.");
        }
        if (append) {
            /* go to the end of the text that is there: its Ctrl-Z, or the end of the file */
            char buf[4096], *z = NULL;
            off_t pos = 0;
            ssize_t n;
            while (text && !z && (n = read(out, buf, sizeof buf)) > 0) {
                z = memchr(buf, 0x1a, (size_t)n);
                pos += z ? z - buf : n;
            }
            if (z && ftruncate(out, pos) < 0)
                z = NULL;
            lseek(out, 0, SEEK_END);
            copied = 1;
        }
        for (; part; part = strtok_r(NULL, "+", &save)) {
            files_of(part, &f, 0);
            for (i = 0; i < f.n; i++) {
                if (same_file(f.name[i], hdst))
                    continue;
                if (copy_data(f.name[i], out, text))
                    copied = 1;
            }
            flist_free(&f);
        }
        if (text && copied && write(out, "\x1a", 1) != 1) /* files of text put together end with Ctrl-Z */
            copied = 0;
        close(out);
        printf("        %d file(s) copied.\r\n", copied);
        free(spec.s);
        return copied ? 0 : 1;
    }

    files_of(src, &f, 0);
    if (!f.n) {
        free(spec.s);
        return fail("The system cannot find the file specified.");
    }
    if (is_dir(hdst)) {
        for (i = 0; i < f.n; i++) {
            snprintf(to, sizeof to, "%s/%s", hdst, base_of(f.name[i]));
            if (copy_file(f.name[i], to))
                copied++;
            else
                st = 1;
        }
    } else if (f.n == 1) {
        if (copy_file(f.name[0], hdst))
            copied++;
        else
            st = fail("The system cannot find the path specified.");
    } else {
        /* many files to one file: one after the other */
        int out = open(hdst, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (out < 0) {
            st = fail("The system cannot find the path specified.");
        } else {
            for (i = 0; i < f.n; i++)
                if (!same_file(f.name[i], hdst) && copy_data(f.name[i], out, text))
                    copied = 1;
            if (text && copied && write(out, "\x1a", 1) != 1)
                copied = 0;
            close(out);
        }
    }
    printf("        %d file(s) copied.\r\n", copied);
    flist_free(&f);
    free(spec.s);
    return st;
}

static u32 do_move(char *args, int ren)
{
    char *a1, *a2, hdst[PATH_MAX], to[PATH_MAX * 2];
    struct flist f = { NULL, 0 };
    u32 st = 0;
    int moved = 0;

    do
        a1 = next_word(&args);
    while (a1 && a1[0] == '/' && strlen(a1) <= 3);
    a2 = next_word(&args);
    if (!a1 || !a2)
        return fail("The syntax of the command is incorrect.");
    files_of(a1, &f, 1);
    if (!f.n)
        return fail("The system cannot find the file specified.");
    if (ren && !strpbrk(a2, "\\/:")) {
        /* REN: the new name is in the folder of the file */
        const char *b = base_of(f.name[0]);
        snprintf(hdst, sizeof hdst, "%.*s%s", (int)(b - f.name[0]), f.name[0], a2);
    } else {
        to_host(a2, hdst, sizeof hdst);
    }
    for (int i = 0; i < f.n; i++) {
        if (is_dir(hdst) && !same_file(f.name[i], hdst))
            snprintf(to, sizeof to, "%s/%s", hdst, base_of(f.name[i]));
        else
            snprintf(to, sizeof to, "%s", hdst);
        if (ren && access(to, F_OK) == 0 && !same_file(f.name[i], to)) {
            st = fail("A duplicate file name exists, or the file cannot be found.");
        } else if (rename(f.name[i], to) == 0) {
            moved++;
        } else if (errno == EXDEV && copy_file(f.name[i], to)) {
            unlink(f.name[i]);
            moved++;
        } else {
            st = fail("The system cannot find the path specified.");
        }
    }
    if (!ren)
        printf("        %d file(s) moved.\r\n", moved);
    flist_free(&f);
    return st;
}

static u32 do_type(char *args)
{
    char *w;
    u32 st = 0;

    while ((w = next_word(&args))) {
        struct flist f = { NULL, 0 };
        files_of(w, &f, 0);
        if (!f.n)
            st = fail("The system cannot find the file specified.");
        fflush(stdout);
        for (int i = 0; i < f.n; i++)
            if (!copy_data(f.name[i], 1, 0))
                st = 1;
        flist_free(&f);
    }
    return st;
}

static u32 do_dir(char *args)
{
    char *w, *pat = NULL, tmp[PATH_MAX], hp[PATH_MAX], g[PATH_MAX];
    int bare = 0, nfiles = 0;
    long long total = 0;
    struct flist f = { NULL, 0 };

    while ((w = next_word(&args))) {
        if (w[0] == '/') {
            if (strstr(w, "/b") || strstr(w, "/B"))
                bare = 1;
        } else {
            pat = w;
        }
    }
    snprintf(tmp, sizeof tmp, "%s", pat ? pat : "*");
    to_host(tmp, hp, sizeof hp);
    if (!has_wild(tmp) && is_dir(hp)) {
        size_t l = strlen(tmp);
        snprintf(tmp + l, sizeof tmp - l, "%s*", tmp[l - 1] == '\\' ? "" : "\\");
    }
    files_of(tmp, &f, 1);
    if (!f.n) {
        flist_free(&f);
        return fail("File Not Found");
    }
    if (!bare) {
        char *bs;
        to_guest(f.name[0], g, sizeof g);
        if ((bs = strrchr(g, '\\')))
            *bs = 0;
        printf("\r\n Directory of %s\r\n\r\n", g);
    }
    for (int i = 0; i < f.n; i++) {
        struct stat st;
        struct tm tm;
        if (bare) {
            printf("%s\r\n", base_of(f.name[i]));
            continue;
        }
        if (stat(f.name[i], &st) < 0)
            continue;
        localtime_r(&st.st_mtime, &tm);
        printf("%02d/%02d/%02d  %02d:%02d%c  ", tm.tm_mon + 1, tm.tm_mday, tm.tm_year % 100, tm.tm_hour % 12 ? tm.tm_hour % 12 : 12,
            tm.tm_min, tm.tm_hour < 12 ? 'a' : 'p');
        if (S_ISDIR(st.st_mode)) {
            printf("       <DIR>          %s\r\n", base_of(f.name[i]));
        } else {
            printf("%20lld %s\r\n", (long long)st.st_size, base_of(f.name[i]));
            nfiles++;
            total += st.st_size;
        }
    }
    if (!bare)
        printf("%16d File(s) %14lld bytes\r\n", nfiles, total);
    flist_free(&f);
    return 0;
}

static u32 do_set(char *args)
{
    char *p = skip_ws(args), *eq;
    size_t len;
    int shown = 0;

    if (*p == '/')
        return fail("ntrun: cmd: SET /A and SET /P are not done");
    len = strlen(p);
    if (*p == '"') { /* set "NAME=value" */
        p++;
        if (len > 1 && p[len - 2] == '"')
            p[len - 2] = 0;
    }
    eq = strchr(p, '=');
    if (eq) {
        *eq = 0;
        env_set(p, eq[1] ? eq + 1 : NULL);
        return 0;
    }
    for (int i = 0; i < ngenv; i++) /* the variables whose names start with the text */
        if (!strncasecmp(genv[i], p, strlen(p)) && genv[i][0] != '=') {
            printf("%s\r\n", genv[i]);
            shown = 1;
        }
    if (!shown && *p)
        return fail("Environment variable not defined");
    return 0;
}

static void local_pop(void)
{
    struct local *l = locals;

    if (!l)
        return;
    for (int i = 0; i < ngenv; i++)
        free(genv[i]);
    free(genv);
    genv = l->env;
    ngenv = l->n;
    if (chdir(l->cwd) < 0)
        perror("ntrun: cmd");
    locals = l->up;
    nlocals--;
    free(l);
}

static void local_push(void)
{
    struct local *l = calloc(1, sizeof *l);

    l->env = malloc(sizeof *l->env * (size_t)(ngenv + 2));
    for (int i = 0; i < ngenv; i++)
        l->env[i] = strdup(genv[i]);
    l->n = ngenv;
    if (!getcwd(l->cwd, sizeof l->cwd))
        snprintf(l->cwd, sizeof l->cwd, ".");
    l->up = locals;
    locals = l;
    nlocals++;
}

static u32 do_goto(char *args)
{
    char *label = next_word(&args);

    if (!bat || !label)
        return 0;
    if (*label == ':')
        label++;
    abort_line = 1;
    if (!strcasecmp(label, "eof")) {
        bat->pc = bat->n;
        return 0;
    }
    for (int i = 0; i < bat->n; i++) {
        const char *p = bat->line[i];
        size_t n;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p != ':')
            continue;
        p++;
        while (*p == ' ' || *p == '\t')
            p++;
        n = strcspn(p, " \t\r\n:+&|<>");
        if (n == strlen(label) && !strncasecmp(p, label, n)) {
            bat->pc = i + 1;
            return 0;
        }
    }
    fprintf(stderr, "The system cannot find the batch label specified - %s\r\n", label);
    bat->done = 1;
    return 1;
}

static u32 do_exit(char *args)
{
    char *w = next_word(&args);
    int b = 0;

    if (w && !strcasecmp(w, "/b")) {
        b = 1;
        w = next_word(&args);
    }
    if (w)
        errorlevel = (u32)strtol(w, NULL, 0);
    if (b && bat) {
        bat->done = 1;
        abort_line = 1;
        return errorlevel;
    }
    fflush(stdout);
    fflush(stderr);
    guest_exit(errorlevel);
}

/* IF [/I] [NOT] EXIST name | ERRORLEVEL n | DEFINED name | text1==text2, then a command.
   With ( ) round the command an ELSE can follow. */
static u32 do_if(char *s)
{
    int neg = 0, nocase = 0, ok = 0;
    char hp[PATH_MAX], *w, *body;

    s = skip_ws(s);
    if (!strncasecmp(s, "/i", 2) && (s[2] == ' ' || s[2] == '\t')) {
        nocase = 1;
        s = skip_ws(s + 2);
    }
    if (is_word(s, "not")) {
        neg = 1;
        s = skip_ws(s + 3);
    }
    if (is_word(s, "exist")) {
        struct flist f = { NULL, 0 };
        s += 5;
        w = next_word(&s);
        if (!w)
            return fail("The syntax of the command is incorrect.");
        files_of(w, &f, 1);
        ok = f.n > 0;
        flist_free(&f);
        if (!ok && !has_wild(w)) { /* NUL, CON: device names */
            struct stat st;
            to_host(w, hp, sizeof hp);
            ok = stat(hp, &st) == 0;
        }
    } else if (is_word(s, "errorlevel")) {
        s += 10;
        w = next_word(&s);
        if (!w)
            return fail("The syntax of the command is incorrect.");
        ok = (s32)errorlevel >= atoi(w);
    } else if (is_word(s, "defined")) {
        s += 7;
        w = next_word(&s);
        ok = w && env_get(w);
    } else {
        /* text1==text2: a text ends at a space or at ==, quotes keep spaces in it */
        char *a = s, *ae, *b, *be;
        int q = 0;
        for (ae = a; *ae && (q || (*ae != ' ' && *ae != '\t' && !(ae[0] == '=' && ae[1] == '='))); ae++)
            if (*ae == '"')
                q = !q;
        b = skip_ws(ae);
        if (b[0] != '=' || b[1] != '=')
            return fail("The syntax of the command is incorrect.");
        b = skip_ws(b + 2);
        for (be = b, q = 0; *be && (q || (*be != ' ' && *be != '\t')); be++)
            if (*be == '"')
                q = !q;
        ok = (ae - a) == (be - b) && !(nocase ? strncasecmp(a, b, (size_t)(ae - a)) : strncmp(a, b, (size_t)(ae - a)));
        s = be;
    }
    if (neg)
        ok = !ok;
    body = skip_ws(s);
    if (*body == '(') {
        char *close = find_close(body), *rest;
        if (close) {
            rest = skip_ws(close + 1);
            if (is_word(rest, "else")) {
                *close = 0;
                return ok ? run_seq(body + 1) : run_seq(rest + 4);
            }
        }
    }
    return ok ? run_seq(body) : 0;
}

/* in a new text: every "%X" of BODY (X is the letter VAR) replaced by VALUE */
static char *for_subst(const char *body, int var, const char *value)
{
    struct text t = { NULL, 0, 0 };

    text_add(&t, "", 0);
    while (*body) {
        if (body[0] == '%' && body[1] == '%' && body[2] == var) {
            text_add(&t, value, strlen(value));
            body += 3;
        } else if (body[0] == '%' && body[1] == var) {
            text_add(&t, value, strlen(value));
            body += 2;
        } else {
            text_add(&t, body++, 1);
        }
    }
    return t.s;
}

/* FOR %X IN (set) DO command */
static u32 do_for(char *s)
{
    char *set, *close, *body, *item, g[PATH_MAX];
    int var;
    u32 st = 0;

    s = skip_ws(s);
    if (*s == '/')
        return fail("ntrun: cmd: FOR /F, /L, /R and /D are not done");
    if (*s != '%')
        return fail("The syntax of the command is incorrect.");
    s++;
    if (*s == '%')
        s++;
    var = *s++;
    s = skip_ws(s);
    if (!var || !is_word(s, "in"))
        return fail("The syntax of the command is incorrect.");
    set = skip_ws(s + 2);
    close = *set == '(' ? find_close(set) : NULL;
    if (!close)
        return fail("The syntax of the command is incorrect.");
    *close = 0;
    set++;
    body = skip_ws(close + 1);
    if (!is_word(body, "do"))
        return fail("The syntax of the command is incorrect.");
    body = skip_ws(body + 2);

    for (item = set; *item && !abort_line;) {
        char *e, save;
        int q = 0;
        while (*item == ' ' || *item == '\t' || *item == ',' || *item == ';')
            item++;
        if (!*item)
            break;
        for (e = item; *e && (q || !strchr(" \t,;", *e)); e++)
            if (*e == '"')
                q = !q;
        save = *e;
        *e = 0;
        if (has_wild(item)) {
            /* the files for a pattern, with the folder as it was written */
            struct flist f = { NULL, 0 };
            char pre[PATH_MAX];
            const char *bs = strrchr(item, '\\');
            snprintf(pre, sizeof pre, "%.*s", bs ? (int)(bs - item + 1) : 0, item);
            files_of(item, &f, 0);
            for (int i = 0; i < f.n && !abort_line; i++) {
                char *line;
                snprintf(g, sizeof g, "%s%s", pre, base_of(f.name[i]));
                line = for_subst(body, var, g);
                st = run_seq(line);
                free(line);
            }
            flist_free(&f);
        } else {
            char *line = for_subst(body, var, item);
            st = run_seq(line);
            free(line);
        }
        *e = save;
        item = e;
    }
    return st;
}

/* ---------------- programs and batch files ---------------- */

static u32 run_batch(const char *hp, const char *name, char *args)
{
    struct batch *b = calloc(1, sizeof *b);
    struct stat st;
    char *data, *p;
    int fd = open(hp, O_RDONLY), depth = nlocals;
    ssize_t len;

    if (fd < 0 || fstat(fd, &st) < 0) {
        free(b);
        return fail("The system cannot find the file specified.");
    }
    data = malloc((size_t)st.st_size + 2);
    len = read(fd, data, (size_t)st.st_size);
    close(fd);
    if (len < 0)
        len = 0;
    data[len] = 0;
    if ((p = memchr(data, 0x1a, (size_t)len))) /* Ctrl-Z: the end of the text */
        *p = 0;
    for (p = data; *p;) {
        char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) : strlen(p), adv = e ? l + 1 : l;
        b->line = realloc(b->line, sizeof *b->line * (size_t)(b->n + 1));
        b->line[b->n] = malloc(l + 1);
        memcpy(b->line[b->n], p, l);
        b->line[b->n][l] = 0;
        while (l && (b->line[b->n][l - 1] == '\r' || b->line[b->n][l - 1] == ' ' || b->line[b->n][l - 1] == '\t'))
            b->line[b->n][--l] = 0;
        b->n++;
        p += adv;
    }
    free(data);
    /* the arguments: space, tab, comma, semicolon and = are between them; quotes stay */
    b->arg[b->narg++] = strdup(name);
    for (p = args; *p && b->narg < 64;) {
        char *e;
        int q = 0;
        while (*p && strchr(" \t,;=", *p))
            p++;
        if (!*p)
            break;
        for (e = p; *e && (q || !strchr(" \t,;=", *e)); e++)
            if (*e == '"')
                q = !q;
        b->arg[b->narg] = malloc((size_t)(e - p) + 1);
        memcpy(b->arg[b->narg], p, (size_t)(e - p));
        b->arg[b->narg++][e - p] = 0;
        p = e;
    }
    b->up = bat;
    bat = b;
    while (b->pc < b->n && !b->done) {
        struct text t = { NULL, 0, 0 };
        char *line, *x;
        int noecho = 0;

        abort_line = 0;
        text_add(&t, b->line[b->pc], strlen(b->line[b->pc]));
        b->pc++;
        /* a line that ends with ( goes on, to the line that closes all the ( ) */
        if (t.n && t.s[t.n - 1] == '(') {
            while (paren_depth(t.s) > 0 && b->pc < b->n) {
                text_add(&t, "&", 1);
                text_add(&t, b->line[b->pc], strlen(b->line[b->pc]));
                b->pc++;
            }
        }
        line = skip_ws(t.s);
        while (*line == '@') {
            noecho = 1;
            line++;
        }
        if (*line && *line != ':') {
            if (echo_on && !noecho) {
                char g[PATH_MAX];
                guest_cwd(g, sizeof g);
                printf("\r\n%s>%s\r\n", g, line);
                fflush(stdout);
            }
            x = expand(line);
            run_seq(x);
            free(x);
        }
        free(t.s);
    }
    while (nlocals > depth)
        local_pop();
    bat = b->up;
    for (int i = 0; i < b->n; i++)
        free(b->line[i]);
    free(b->line);
    for (int i = 0; i < b->narg; i++)
        free(b->arg[i]);
    free(b);
    abort_line = 0;
    return errorlevel;
}

/* the batch file for a name: in the current folder, then in the folders of PATH */
static int find_batch(const char *name, char *out, size_t n)
{
    static const char *const ext[] = { "", ".bat", ".cmd" };
    const char *dot = strrchr(name, '.'), *path;
    int hasext = dot && !strpbrk(dot, "\\/");
    char g[PATH_MAX], hdir[PATH_MAX];

    if (hasext && strcasecmp(dot, ".bat") && strcasecmp(dot, ".cmd"))
        return 0;
    for (int k = hasext ? 0 : 1; k < (hasext ? 1 : 3); k++) {
        snprintf(g, sizeof g, "%s%s", name, ext[k]);
        if (strpbrk(name, "\\/:")) {
            to_host(g, out, n);
            if (access(out, R_OK) == 0 && !is_dir(out))
                return 1;
            continue;
        }
        if (find_file(".", g, out, n) && !is_dir(out))
            return 1;
        for (path = env_get("PATH"); path && *path;) {
            size_t l = strcspn(path, ";");
            char gdir[PATH_MAX];
            snprintf(gdir, sizeof gdir, "%.*s", (int)l, path);
            if (gdir[0]) {
                to_host(gdir, hdir, sizeof hdir);
                if (find_file(hdir, g, out, n) && !is_dir(out))
                    return 1;
            }
            path += l;
            if (*path)
                path++;
        }
    }
    return 0;
}

/* a program or a batch file. CALLED: with CALL, so a batch file comes back to the one that runs. */
static u32 run_external(char *s, int called)
{
    char name[PATH_MAX], hp[PATH_MAX], *copy = strdup(s), *rest = copy, *w, *args;
    const char *dot;
    size_t off, len = strlen(s);
    int isbat;

    w = next_word(&rest);
    snprintf(name, sizeof name, "%s", w ? w : "");
    off = len - strlen(rest); /* the place of the text after the name */
    free(copy);
    args = s + off;
    if (!name[0])
        return 0;
    dot = strrchr(name, '.');
    isbat = dot && (!strcasecmp(dot, ".bat") || !strcasecmp(dot, ".cmd"));

    if (cmd_is_name(name)) {
        errorlevel = run_program("cmd.exe", s);
        return errorlevel;
    }
    if (!isbat && find_program(name, hp, sizeof hp)) {
        errorlevel = run_program(hp, s);
        return errorlevel;
    }
    if (!isbat && !(dot && !strpbrk(dot, "\\/"))) {
        char com[PATH_MAX + 8];
        snprintf(com, sizeof com, "%s.com", name);
        if (find_program(com, hp, sizeof hp)) {
            errorlevel = run_program(hp, s);
            return errorlevel;
        }
    }
    if (find_batch(name, hp, sizeof hp)) {
        u32 st = run_batch(hp, name, args);
        if (bat && !called) { /* no CALL: the batch file that started it does not go on */
            bat->done = 1;
            abort_line = 1;
        }
        return st;
    }
    fprintf(stderr, "'%s' is not recognized as an internal or external command,\r\noperable program or batch file.\r\n", name);
    errorlevel = 1;
    return 1;
}

static const char *const builtins[] = {
    "break", "call", "cd", "chdir", "cls", "color", "copy", "del", "dir", "echo", "endlocal", "erase", "exit",
    "goto", "md", "mkdir", "move", "path", "pause", "popd", "prompt", "pushd", "rd", "rem", "ren", "rename",
    "rmdir", "set", "setlocal", "shift", "title", "type", "ver", "verify", NULL
};

/* one command without redirections, operators or ( ) */
static u32 run_simple(char *s)
{
    size_t len = 0;
    char *args, *w, hp[PATH_MAX], g[PATH_MAX];
    const char *name = NULL;
    u32 st = 0;

    if (*s == ':')
        return 0; /* a label */
    if (strncasecmp(s, "echo", 4)) /* ECHO keeps the spaces at the end */
        for (char *e = s + strlen(s); e > s && (e[-1] == ' ' || e[-1] == '\t');)
            *--e = 0;
    if (isalpha((unsigned char)s[0]) && s[1] == ':' && !s[2])
        return 0; /* "X:" changes the drive: every drive is the same one here */
    while (isalpha((unsigned char)s[len]))
        len++;
    if (len && (!s[len] || strchr(" \t/\\.:=,;(\"", s[len])))
        for (int i = 0; builtins[i]; i++)
            if (strlen(builtins[i]) == len && !strncasecmp(s, builtins[i], len)) {
                name = builtins[i];
                break;
            }
    /* "copy.exe" is a program, not the command; "echo." and "cd.." are the commands */
    if (name && s[len] == '.' && isalpha((unsigned char)s[len + 1]) && strcmp(name, "echo"))
        name = NULL;
    if (!name) {
        uncaret(s);
        return run_external(s, 0);
    }
    args = s + len;
    if (!strcmp(name, "echo"))
        return do_echo(args);
    if (!strcmp(name, "rem"))
        return 0;
    uncaret(args);
    if (!strcmp(name, "call")) {
        char *copy = strdup(args), *rest = copy;
        w = next_word(&rest);
        if (w && find_batch(w, hp, sizeof hp) && !find_program(w, g, sizeof g)) {
            char *a = args + (rest - copy);
            if (a > args + strlen(args))
                a = args + strlen(args);
            snprintf(g, sizeof g, "%s", w);
            free(copy);
            return run_batch(hp, g, a);
        }
        free(copy);
        return run_seq(args);
    }
    if (!strcmp(name, "cd") || !strcmp(name, "chdir"))
        st = do_cd(args);
    else if (!strcmp(name, "md") || !strcmp(name, "mkdir"))
        st = do_md(args);
    else if (!strcmp(name, "rd") || !strcmp(name, "rmdir"))
        st = do_rd(args);
    else if (!strcmp(name, "del") || !strcmp(name, "erase"))
        st = do_del(args);
    else if (!strcmp(name, "copy"))
        st = do_copy(args);
    else if (!strcmp(name, "move"))
        st = do_move(args, 0);
    else if (!strcmp(name, "ren") || !strcmp(name, "rename"))
        st = do_move(args, 1);
    else if (!strcmp(name, "type"))
        st = do_type(args);
    else if (!strcmp(name, "dir"))
        st = do_dir(args);
    else if (!strcmp(name, "pushd")) {
        if (ndirstack < 32 && getcwd(hp, sizeof hp)) {
            dirstack[ndirstack++] = strdup(hp);
            st = do_cd(args);
            if (st)
                free(dirstack[--ndirstack]);
        }
    } else if (!strcmp(name, "popd")) {
        if (ndirstack) {
            st = chdir(dirstack[--ndirstack]) < 0;
            free(dirstack[ndirstack]);
        }
    } else {
        /* the commands that do not change ERRORLEVEL when they do their work */
        if (!strcmp(name, "set"))
            return do_set(args);
        if (!strcmp(name, "goto"))
            return do_goto(args);
        if (!strcmp(name, "exit"))
            return do_exit(args);
        if (!strcmp(name, "shift")) {
            if (bat)
                bat->shift++;
        } else if (!strcmp(name, "setlocal")) {
            local_push();
        } else if (!strcmp(name, "endlocal")) {
            local_pop();
        } else if (!strcmp(name, "path")) {
            w = skip_ws(args);
            if (*w == '=')
                w = skip_ws(w + 1);
            if (!*w)
                printf("PATH=%s\r\n", env_get("PATH") ? env_get("PATH") : "(null)");
            else
                env_set("PATH", !strcmp(w, ";") ? NULL : w);
        } else if (!strcmp(name, "pause")) {
            printf("Press any key to continue . . . ");
            fflush(stdout);
            if (isatty(0)) {
                char c;
                while (read(0, &c, 1) == 1 && c != '\n')
                    ;
            }
            printf("\r\n");
        } else if (!strcmp(name, "ver")) {
            printf("\r\nWindows NT Version 3.51 (ntrun)\r\n\r\n");
        }
        return 0;
    }
    errorlevel = st;
    return st;
}

/* ---------------- lines ---------------- */

/* a command with its redirections: a ( ) group or a simple command */
static u32 run_command(char *s)
{
    struct redir r = { 0, { 0 }, { 0 } };
    u32 st;

    while (*s == ' ' || *s == '\t' || *s == '@')
        s++;
    if (!*s)
        return 0;
    if (*s == '(') {
        char *close = find_close(s);
        if (!close)
            return fail("The syntax of the command is incorrect.");
        *close = 0;
        if (!redirect(close + 1, &r)) {
            redir_undo(&r);
            return 1;
        }
        st = run_seq(s + 1);
    } else {
        if (!redirect(s, &r)) {
            redir_undo(&r);
            errorlevel = 1;
            return 1;
        }
        st = run_simple(s);
    }
    redir_undo(&r);
    return st;
}

/* commands with | between them: the output of one is the input of the next */
static u32 run_pipeline(char *s)
{
    char *bar = find_op(s, 1);
    int fd[2], save0, wst;
    pid_t pid;
    u32 st;

    if (!bar)
        return run_command(s);
    *bar = 0;
#ifdef _WIN32
    /* Not inherited: the first command must not hold the read end of its own pipe (it would wait for
       a second command that has stopped reading). dup2 makes the copies on 0 and 1 inheritable. */
    if (_pipe(fd, 65536, _O_BINARY | _O_NOINHERIT) < 0)
#else
    if (pipe(fd) < 0)
#endif
        return 1;
    fflush(stdout);
    fflush(stderr);
#ifdef _WIN32
    {
        /* no fork: the first command runs in a new ntrun, with the pipe as its output */
        int save1 = dup(1);
        char *line = malloc(strlen(s) + 16);
        intptr_t proc;
        (void)pid;
        (void)wst;
        sprintf(line, "cmd /c %s", s);
        dup2(fd[1], 1);
        close(fd[1]);
        proc = start_program("cmd.exe", line);
        dup2(save1, 1);
        close(save1);
        free(line);
        save0 = dup(0);
        dup2(fd[0], 0);
        close(fd[0]);
        st = run_pipeline(bar + 1);
        fflush(stdout);
        dup2(save0, 0);
        close(save0);
        if (proc != -1)
            wait_program(proc);
        return st;
    }
#else
    pid = fork();
    if (pid < 0)
        return 1;
    if (pid == 0) {
        unsetenv("NTRUN_EXITFD"); /* this is not the process whose exit code the parent waits for */
        close(fd[0]);
        dup2(fd[1], 1);
        close(fd[1]);
        st = run_command(s);
        fflush(stdout);
        fflush(stderr);
        _exit((int)(st & 0xff));
    }
    close(fd[1]);
    save0 = dup(0);
    dup2(fd[0], 0);
    close(fd[0]);
    st = run_pipeline(bar + 1);
    fflush(stdout);
    dup2(save0, 0);
    close(save0);
    while (waitpid(pid, &wst, 0) < 0 && errno == EINTR)
        ;
    return st;
#endif
}

/* commands with & (then), && (then, if it was good) and || (then, if it failed) between them.
   IF and FOR take all the rest of the line. */
static u32 run_seq(char *s)
{
    u32 st = 0;
    int prev = '&';

    while (!abort_line) {
        char *op;
        int next = 0, skip;

        while (*s == ' ' || *s == '\t' || *s == '@')
            s++;
        if (!*s)
            break;
        skip = (prev == 'A' && st) || (prev == 'O' && !st);
        if (is_word(s, "if") || is_word(s, "for")) {
            if (skip)
                break;
            return is_word(s, "if") ? do_if(s + 2) : do_for(s + 3);
        }
        op = find_op(s, 0);
        if (op) {
            if (op[0] == '&' && op[1] == '&') {
                next = 'A';
                *op = 0;
                op += 2;
            } else if (op[0] == '|') {
                next = 'O';
                *op = 0;
                op += 2;
            } else {
                next = '&';
                *op = 0;
                op += 1;
            }
        }
        if (!skip)
            st = run_pipeline(s);
        if (!op)
            break;
        prev = next;
        s = op;
    }
    return st;
}

/* The command processor. CMDLINE is all of its command line ("cmd /c echo hello").
   Without a command it reads lines from the standard input. The result is the exit code. */
u32 cmd_shell(const char *cmdline)
{
    char *copy = strdup(cmdline), *rest = copy, *w, *c = NULL, line[8192];

    (void)next_word(&rest); /* the name of the program */
    for (;;) {
        char *save = rest;
        w = next_word(&rest);
        if (!w)
            break;
        if (!strcasecmp(w, "/c") || !strcasecmp(w, "/k")) {
            c = strdup(cmdline + (rest - copy));
            break;
        }
        if (!strcasecmp(w, "/q")) {
            echo_on = 0;
        } else if (w[0] != '/') { /* no /c: the rest is the command */
            c = strdup(cmdline + (save - copy));
            break;
        }
    }
    free(copy);
    if (c) {
        size_t l = strlen(c);
        char *x, *p = c;
        /* "cmd /c "text"": the quotes round all of the text go away */
        if (l >= 2 && c[0] == '"' && c[l - 1] == '"' && strchr(c + 1, '"') == c + l - 1) {
            c[l - 1] = 0;
            p = c + 1;
        }
        x = expand(p);
        run_seq(x);
        free(x);
        free(c);
        return errorlevel;
    }
    for (;;) {
        char *x;
        size_t l;
        if (isatty(0)) {
            char g[PATH_MAX];
            guest_cwd(g, sizeof g);
            printf("%s>", g);
        }
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin))
            break;
        l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r'))
            line[--l] = 0;
        abort_line = 0;
        x = expand(line);
        run_seq(x);
        free(x);
    }
    return errorlevel;
}
