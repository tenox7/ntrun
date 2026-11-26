/* mapview.c: a view of a file that the program can write must be the file itself: what is
   written through the view is in the file at once, what is written to the file is in the view,
   and a second view of the file shows the same bytes. */
#include <stdio.h>
#include <string.h>
#include <windows.h>

static void text(const char *what, const char *p, int n)
{
    char b[64];

    memcpy(b, p, (size_t)n);
    b[n] = 0;
    printf("%s [%s]\n", what, b);
}

int main(void)
{
    HANDLE f, g, m;
    char *v, *w, buf[64];
    DWORD n;

    f = CreateFile("map.tmp", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, NULL);
    m = CreateFileMapping(f, NULL, PAGE_READWRITE, 0, 0x20000, NULL);
    v = (char *)MapViewOfFile(m, FILE_MAP_WRITE, 0, 0, 0x20000);
    if (f == INVALID_HANDLE_VALUE || !m || !v) {
        printf("cannot map\n");
        return 1;
    }
    strcpy(v, "written through the view");

    /* 1: the file has what the view has, with no flush */
    g = CreateFile("map.tmp", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    memset(buf, 0, sizeof buf);
    ReadFile(g, buf, 24, &n, NULL);
    text("1 file sees the view:", buf, 24);
    CloseHandle(g);

    /* 2: the view has what WriteFile writes */
    SetFilePointer(f, 100, NULL, FILE_BEGIN);
    WriteFile(f, "from WriteFile", 14, &n, NULL);
    text("2 view sees WriteFile:", v + 100, 14);

    /* 3: a second view shows the same bytes, and a write through it shows in the first */
    w = (char *)MapViewOfFile(m, FILE_MAP_WRITE, 0, 0, 0x20000);
    text("3 second view:", w, 24);
    w[200] = 'Z';
    printf("4 first view sees: %c\n", v[200]);
    UnmapViewOfFile(w);

    UnmapViewOfFile(v);
    CloseHandle(m);
    printf("5 size %d\n", (int)GetFileSize(f, NULL));
    CloseHandle(f);

    /* 4: after the views are gone, the file has it all */
    g = CreateFile("map.tmp", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    memset(buf, 0, sizeof buf);
    ReadFile(g, buf, 24, &n, NULL);
    text("6 file:", buf, 24);
    SetFilePointer(g, 100, NULL, FILE_BEGIN);
    ReadFile(g, buf, 14, &n, NULL);
    text("7 file:", buf, 14);
    SetFilePointer(g, 200, NULL, FILE_BEGIN);
    ReadFile(g, buf, 1, &n, NULL);
    printf("8 file: %c\n", buf[0]);
    CloseHandle(g);
    DeleteFile("map.tmp");
    return 0;
}
