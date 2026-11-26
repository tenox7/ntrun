#!/bin/sh
# run.sh [TOOLS [CPU ...]]: build the tests with Visual C++ 4.x for each CPU under ntrun,
# run the programs under ntrun too, and compare what they print with the *.out files
# (the output of the same programs on real Windows NT 3.51; NAME.out.CPU where a CPU differs).
#
# TOOLS is a folder with the tool sets, in the layout of the ntsdk image
# (https://github.com/tenox7/ntsdk). The default is /opt, the folder in that image.
#   nt40/CPU/msdev   bin, include, lib and redist of Visual C++ 4.x for mips, alpha, ppc and x86
#   sys/CPU          CRTDLL.DLL of Windows NT for that CPU
#   nt50/axp64       the tools for 64-bit Alpha (CPU axp64): 32-bit Alpha programs. ntrun cannot
#                    run what they make, so the test reads the headers of the files they make.
#   nt52/ia64        the tools for Itanium (CPU ia64): x86 programs. The same kind of test.
# TOOLS can be the folder tools of the ntsdk repository: x86 is then ../nt40sdk/msdev.
# NTRUN is the ntrun to test (default: the one in the folder above this one).
# KEEP=1 keeps the work folder. The work folder is $WORK (default: /tmp/ntrun-tests).

here=$(cd "$(dirname "$0")" && pwd)
ntrun=${NTRUN:-$here/../ntrun}
# MSYS2 with a native Windows ntrun.exe: paths for ntrun have a drive (C:/dir), as the host has them
if command -v cygpath >/dev/null 2>&1; then
    [ -f "$ntrun.exe" ] && ntrun=$ntrun.exe
    ntrun=$(cygpath -m "$ntrun")
    hostpath() { cygpath -m "$1"; }
    win() { cygpath -w "$1"; }
    # with a folder as drive C: the host's C:\dir is Z:\c\dir for the tools
    winz() { m=$(cygpath -m "$1"); d=$(echo "${m%%:*}" | tr A-Z a-z); echo "Z:/$d${m#*:}" | tr / '\\'; }
    psep=';'
else
    hostpath() { echo "$1"; }
    win() { echo "Z:$1" | tr / '\\'; }
    winz() { win "$1"; }
    psep=:
fi
tools=${1:-/opt}
[ -d "$tools/nt40" ] || { echo "usage: run.sh [TOOLS [mips alpha ppc x86]]" >&2; exit 2; }
tools=$(hostpath "$(cd "$tools" && pwd)")
[ $# -eq 0 ] || shift
[ $# -gt 0 ] || set -- mips alpha ppc x86 axp64 ia64
work=$(hostpath "${WORK:-/tmp/ntrun-tests}")
fail=0
total=0


# run_limited SECONDS COMMAND...: the command is stopped after that time (macOS has no timeout(1)).
# A command that had to be stopped writes a line about it to its own output, so that its test fails.
run_limited() {
    t=$1
    shift
    flag=run_limited.stopped
    rm -f "$flag"
    "$@" &
    p=$!
    # the watch ends by itself soon after the command (it is not killed: a shell would report it)
    ( i=0
      while [ "$i" -lt "$t" ] && kill -0 "$p" 2> /dev/null; do
          sleep 1
          i=$((i + 1))
      done
      if kill -0 "$p" 2> /dev/null; then : > "$flag"; kill "$p" 2> /dev/null; fi ) > /dev/null 2>&1 &
    k=$!
    wait "$p"
    r=$?
    wait "$k" 2> /dev/null
    if [ -f "$flag" ]; then
        rm -f "$flag"
        echo "stopped: it did not end in $t seconds"
        return 1
    fi
    return $r
}

check() { # check NAME CPU: compare NAME.txt with the expected output
    exp=$here/$1.out
    [ -f "$here/$1.out.$2" ] && exp=$here/$1.out.$2
    total=$((total + 1))
    if tr -d '\r' < "$1.txt" | cmp -s - "$exp"; then
        echo "  ok    $1"
    else
        echo "  FAIL  $1 (see $work/$2/$1.txt, expected $exp)"
        fail=$((fail + 1))
    fi
}

# the command processor: a batch file. It needs no tools. The expected output is what CMD.EXE
# of Windows NT 3.51 printed for the same file.
echo "== cmd"
d=$work/cmd
rm -rf "$d"
mkdir -p "$d"
cp "$here"/cmdtest.bat "$d"/
cd "$d" || exit 1
$ntrun cmd /c cmdtest.bat one "two words" three > cmdtest.txt 2>&1
check cmdtest cmd
# a drive letter for a host folder
NTRUN_DRIVE_Q=$d $ntrun cmd /c "cd& if exist q:\\cmdtest.bat echo found& md sub& cd sub& cd& cd ..& rd sub" > drive.txt 2>&1
check drive cmd
# a pipeline whose second command ends without reading, with a first command that writes 139264 bytes:
# it must end (a first command that holds its own pipe waits for ever)
cp "$here"/pipe.bat "$d"/
run_limited ${PIPE_LIMIT:-60} $ntrun cmd /c pipe.bat > pipe.txt 2>&1
check pipe cmd

for cpu in "$@"; do
    echo "== $cpu"
    d=$work/$cpu
    rm -rf "$d"
    mkdir -p "$d"
    cp "$here"/*.c "$here"/*.cpp "$here"/*.rc "$here"/*.mak "$here"/*.def "$here"/*.mc "$here"/*.h "$d"/ 2>/dev/null
    cd "$d" || exit 1
    if [ "$cpu" = axp64 ]; then
        a=$tools/nt50/axp64
        if [ ! -d "$a" ]; then
            echo "  left out: $tools has no nt50/axp64"
            continue
        fi
        # C++ with exceptions, then mc, rc, lib, a DLL and a program, all for 64-bit Alpha (-Ap64)
        NTRUN_DRIVE_C=$a
        NTRUN_PATH='C:\mssdk\bin\win64;C:\mssdk\bin;C:\vc98\bin'
        INCLUDE='C:\mssdk\include;C:\mssdk\include\win64\crt'
        LIB='C:\mssdk\lib\win64;C:\mssdk\lib\win64\crt'
        NTRUN_DLLPATH=$a/vc98/redist$psep$tools/sys/alpha
        TMP="$(winz "$d")"
        CL=-Ap64
        export NTRUN_DRIVE_C NTRUN_PATH INCLUDE LIB NTRUN_DLLPATH TMP CL
        $ntrun cl -nologo -GX eh.cpp > eh.build 2>&1
        $ntrun nmake -nologo -f tools.mak > tools.build 2>&1
        for f in eh.exe app.exe mydll.dll; do
            $ntrun link -dump -headers $f 2>&1 | tr -d '\r' | sed -n "s/^ *\\(.*machine (.*\\)$/$f: \\1/p; s/^ *\\(.*magic #.*\\)$/$f: \\1/p"
        done > axp64.txt
        unset NTRUN_DRIVE_C CL
        check axp64 $cpu
        continue
    fi
    if [ "$cpu" = ia64 ]; then
        a=$tools/nt52/ia64
        if [ ! -d "$a" ]; then
            echo "  left out: $tools has no nt52/ia64"
            continue
        fi
        # the same for Itanium. Each link needs bufferoverflowu.lib of this SDK (the -GS check).
        NTRUN_DRIVE_C=$a
        NTRUN_PATH='C:\mssdk\bin\win64;C:\mssdk\bin'
        INCLUDE='C:\mssdk\include\crt;C:\mssdk\include'
        LIB='C:\mssdk\lib\ia64'
        NTRUN_DLLPATH=$tools/sys/x86
        TMP="$(winz "$d")"
        LINK=bufferoverflowu.lib
        export NTRUN_DRIVE_C NTRUN_PATH INCLUDE LIB NTRUN_DLLPATH TMP LINK
        $ntrun cl -nologo -EHsc eh.cpp > eh.build 2>&1
        $ntrun nmake -nologo -f tools.mak > tools.build 2>&1
        for f in eh.exe app.exe mydll.dll; do
            $ntrun link -dump -headers $f 2>&1 | tr -d '\r' | sed -n "s/^ *\\(.*machine (.*\\)$/$f: \\1/p; s/^ *\\(.*magic #.*\\)$/$f: \\1/p"
        done > ia64.txt
        unset NTRUN_DRIVE_C LINK
        check ia64 $cpu
        continue
    fi
    v=$tools/nt40/$cpu/msdev
    [ -d "$v" ] || v=$tools/../nt40sdk/msdev
    INCLUDE="$(win "$v/include")"
    LIB="$(win "$v/lib")"
    TMP="$(win "$d")"
    NTRUN_DLLPATH=$v/redist$psep$tools/sys/$cpu
    NTRUN_PATH="$(win "$v/bin")"
    unset NTRUN_DRIVE_C
    export INCLUDE LIB TMP NTRUN_DLLPATH NTRUN_PATH
    CPU=$(echo "$cpu" | tr a-z A-Z)
    export CPU
    cl="$ntrun $v/bin/CL.EXE -nologo"

    # exceptions, thread-local data
    for t in seh jmpex tls; do
        $cl $t.c > $t.build 2>&1 && $ntrun $t.exe > $t.txt 2>&1
        check $t $cpu
    done
    # threads
    $cl -MT thread.c > thread.build 2>&1 && $ntrun thread.exe > thread.txt 2>&1
    check thread $cpu
    # integer and floating point arithmetic; the cases with no ordinary result
    $cl -O2 num.c > num.build 2>&1 && $ntrun num.exe > num.txt 2>&1
    check num $cpu
    $cl fpx.c > fpx.build 2>&1 && $ntrun fpx.exe > fpx.txt 2>&1
    check fpx $cpu
    # the math functions of the C library: the bits of 12000 results
    $cl -O2 math.c > math.build 2>&1 && $ntrun math.exe > math.txt 2>&1
    check math $cpu
    # the x87: each instruction in each precision and rounding mode (the source has x86 instructions)
    if [ "$cpu" = x86 ]; then
        $cl -Od x87.c > x87.build 2>&1 && $ntrun x87.exe > x87.txt 2>&1
        check x87 $cpu
    fi
    # C++ exceptions
    $cl -GX eh.cpp > eh.build 2>&1 && $ntrun eh.exe > eh.txt 2>&1
    check eh $cpu
    # the MD5 and SHA-1 sums of the system
    $cl hash.c > hash.build 2>&1 && $ntrun hash.exe > hash.txt 2>&1
    check hash $cpu
    # a pipe from a child process: _popen, and a pipe of CreatePipe as the output of the child
    $cl popen.c > popen.build 2>&1 && $ntrun popen.exe > popen.txt 2>&1
    check popen $cpu
    # a view of a file that can be written is the file: the views, ReadFile and WriteFile agree
    $cl mapview.c > mapview.build 2>&1 && $ntrun mapview.exe > mapview.txt 2>&1
    check mapview $cpu
    # mc, rc, lib, a DLL and a program that uses them
    if [ -f tools.mak ]; then
        $ntrun $v/bin/NMAKE.EXE -nologo -f tools.mak > tools.build 2>&1 && $ntrun app.exe > tools.txt 2>&1
        check tools $cpu
    fi
done
echo "$((total - fail)) of $total tests passed"
[ -n "$KEEP" ] || [ $fail -ne 0 ] || rm -rf "$work"
[ $fail -eq 0 ]
