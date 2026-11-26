/* popen.c: a pipe from a child process. The host must pass a pipe to the child process, and
   the child must write to it. First _popen (it makes the pipe, and a copy of its write end
   for the child), then the pipe of CreatePipe that the program gives to the child as it is. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static void show(const char *name, FILE *f)
{
    char line[256];
    int n = 0;

    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        printf("%s %d: [%s]\n", name, ++n, line);
    }
}

int main(void)
{
    FILE *f;

    f = _popen("cmd /c echo one&echo two", "r");
    if (!f) {
        printf("_popen failed\n");
        return 1;
    }
    show("lines", f);
    printf("exit code %d\n", _pclose(f));

    f = _popen("cmd /c exit 3", "r");
    if (!f) {
        printf("_popen failed\n");
        return 1;
    }
    show("none", f);
    printf("exit code %d\n", _pclose(f));

    /* the handle of CreatePipe, inheritable, as the standard output of the child */
    {
        SECURITY_ATTRIBUTES sa;
        STARTUPINFO si;
        PROCESS_INFORMATION pi;
        HANDLE rd, wr;
        char cmd[512], buf[256];
        DWORD n, total = 0, code = 99;
        const char *comspec = getenv("COMSPEC");

        sa.nLength = sizeof sa;
        sa.lpSecurityDescriptor = NULL;
        sa.bInheritHandle = TRUE;
        if (!CreatePipe(&rd, &wr, &sa, 0)) {
            printf("CreatePipe failed\n");
            return 1;
        }
        memset(&si, 0, sizeof si);
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = wr;
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        sprintf(cmd, "%s /c echo captured", comspec ? comspec : "cmd.exe");
        if (!CreateProcess(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
            printf("CreateProcess failed\n");
            return 1;
        }
        CloseHandle(wr);
        while (ReadFile(rd, buf + total, (DWORD)(sizeof buf - 1 - total), &n, NULL) && n)
            total += n;
        buf[total] = 0;
        buf[strcspn(buf, "\r\n")] = 0;
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &code);
        printf("captured [%s] exit code %d\n", buf, (int)code);
    }
    return 0;
}
