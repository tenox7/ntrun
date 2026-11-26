#!/bin/sh
# ntsdk-env.sh: source it, then call "ntsdk_env SDK CPU" to set the environment of one SDK for
# one CPU, as the ntsdk script of the ntsdk repository does, with no Docker.
#   SDK: nt31 nt35 nt40 nt50 nt52     CPU: x86 mips alpha ppc axp64 ia64
# It sets NTRUN_DRIVE_C (and D: for the headers that the image shares by symbolic links),
# NTRUN_PATH, NTRUN_DLLPATH, INCLUDE, LIB, CL, CPU, OS and so on, and NTRUN to the ntrun to run.
# NTSDK_ROOT is the folder of the ntsdk repository (default: the ntsdk repository next to the folder of
# this project, ../ntsdk).
# Needs a native ntrun.exe (MSYS2 UCRT64, ntrun built with make) or a Unix ntrun.

ntsdk_here=$(cd "$(dirname "${BASH_SOURCE:-$0}")" && pwd)
NTSDK_ROOT=${NTSDK_ROOT:-$ntsdk_here/../../../ntsdk}

if command -v cygpath > /dev/null 2>&1; then
    ntsdk_host() { cygpath -m "$1"; }
    # the host's drive C: is Z:\c below the drive of the host root, as ntrun has it with drives set
    ntsdk_win() { m=$(cygpath -m "$1"); d=$(echo "${m%%:*}" | tr A-Z a-z); echo "Z:/$d${m#*:}" | tr / '\\'; }
    ntsdk_sep=';'
else
    ntsdk_host() { echo "$1"; }
    ntsdk_win() { echo "Z:$1" | tr / '\\'; }
    ntsdk_sep=:
fi
NTSDK_ROOT=$(ntsdk_host "$(cd "$NTSDK_ROOT" && pwd)")
if [ -z "$NTRUN" ]; then
    NTRUN=$ntsdk_here/../../ntrun
    [ -f "$NTRUN.exe" ] && NTRUN=$NTRUN.exe
    NTRUN=$(ntsdk_host "$NTRUN")
fi

ntsdk_env() {
    sdk=$1
    cpu=$2
    t=$NTSDK_ROOT/tools
    s=$ntsdk_sep
    unset NTRUN_DRIVE_D CL APPVER TARGETOS LINK
    NTSDK=$sdk
    NTCPU=$cpu
    case $sdk-$cpu in
    nt31-x86)
        NTRUN_DRIVE_C=$NTSDK_ROOT/nt31sdk
        NTRUN_PATH='C:\mstools\bin'
        INCLUDE='C:\mstools\h'
        LIB='C:\mstools\lib'
        NTRUN_DLLPATH=$t/sys/x86
        ;;
    nt31-*)
        NTRUN_DRIVE_C=$t/nt31/$cpu
        NTRUN_DRIVE_D=$NTSDK_ROOT/nt31sdk/mstools # the headers are the same for each CPU
        NTRUN_PATH='C:\mstools\bin'
        INCLUDE='D:\h'
        LIB='C:\mstools\lib'
        NTRUN_DLLPATH=$t/sys/$cpu
        ;;
    nt35-x86)
        NTRUN_DRIVE_C=$NTSDK_ROOT/nt35sdk
        NTRUN_PATH='C:\msvc20\bin;C:\mstools\bin'
        INCLUDE='C:\msvc20\include;C:\msvc20\mfc\include;C:\mstools\h'
        LIB='C:\msvc20\lib;C:\msvc20\mfc\lib;C:\mstools\lib'
        NTRUN_DLLPATH=$NTSDK_ROOT/nt35sdk/msvc20/redist$s$t/sys/x86
        ;;
    nt35-mips)
        NTRUN_DRIVE_C=$t/nt35/mips
        NTRUN_PATH='C:\msvc20\bin'
        INCLUDE='C:\msvc20\include;C:\msvc20\mfc\include'
        LIB='C:\msvc20\lib;C:\msvc20\mfc\lib'
        NTRUN_DLLPATH=$t/nt35/mips/msvc20/redist$s$t/sys/mips
        ;;
    nt35-alpha)
        NTRUN_DRIVE_C=$t/nt35/alpha
        NTRUN_PATH='C:\mstools\bin'
        INCLUDE='C:\mstools\h;C:\mstools\h\crt;C:\mstools\idl'
        LIB='C:\mstools\lib;C:\mstools\lib\crt'
        NTRUN_DLLPATH=$t/sys/alpha
        ;;
    nt40-x86)
        NTRUN_DRIVE_C=$NTSDK_ROOT/nt40sdk
        NTRUN_PATH='C:\mstools\bin;C:\msdev\bin'
        INCLUDE='C:\mstools\include;C:\msdev\include;C:\msdev\mfc\include'
        LIB='C:\mstools\lib;C:\msdev\lib;C:\msdev\mfc\lib'
        NTRUN_DLLPATH=$NTSDK_ROOT/nt40sdk/msdev/redist$s$t/sys/x86
        ;;
    nt40-*)
        NTRUN_DRIVE_C=$t/nt40/$cpu
        NTRUN_DRIVE_D=$NTSDK_ROOT/nt40sdk/mstools/include
        NTRUN_PATH='C:\mstools\bin;C:\msdev\bin'
        INCLUDE='D:\;C:\msdev\include;C:\msdev\mfc\include'
        LIB='C:\mstools\lib;C:\msdev\lib;C:\msdev\mfc\lib'
        NTRUN_DLLPATH=$t/nt40/$cpu/msdev/redist$s$t/sys/$cpu
        ;;
    nt50-axp64)
        NTRUN_DRIVE_C=$t/nt50/axp64
        NTRUN_PATH='C:\mssdk\bin\win64;C:\mssdk\bin;C:\vc98\bin'
        INCLUDE='C:\mssdk\include;C:\mssdk\include\win64\crt;C:\mssdk\include\win64\mfc;C:\mssdk\include\win64\atl;C:\vc98\include'
        LIB='C:\mssdk\lib\win64;C:\mssdk\lib\win64\crt;C:\mssdk\lib\win64\mfc;C:\mssdk\lib\win64\atl'
        NTRUN_DLLPATH=$t/nt50/axp64/vc98/redist$s$t/sys/alpha
        CL=-Ap64
        ;;
    nt52-ia64)
        NTRUN_DRIVE_C=$t/nt52/ia64
        NTRUN_PATH='C:\mssdk\bin\win64;C:\mssdk\bin'
        INCLUDE='C:\mssdk\include\crt;C:\mssdk\include\crt\sys;C:\mssdk\include\mfc;C:\mssdk\include\atl;C:\mssdk\include'
        LIB='C:\mssdk\lib\ia64;C:\mssdk\lib\ia64\mfc'
        NTRUN_DLLPATH=$t/sys/x86
        APPVER=5.02
        TARGETOS=WINNT
        ;;
    *)
        echo "ntsdk-env: there is no $sdk for $cpu" >&2
        return 1
        ;;
    esac
    case $cpu in
    x86) CPU=i386 PROCESSOR_ARCHITECTURE=x86 ;;
    mips) CPU=MIPS PROCESSOR_ARCHITECTURE=MIPS ;;
    alpha) CPU=ALPHA PROCESSOR_ARCHITECTURE=ALPHA ;;
    ppc) CPU=PPC PROCESSOR_ARCHITECTURE=PPC ;;
    axp64) CPU=ALPHA64 PROCESSOR_ARCHITECTURE=ALPHA ;;
    ia64) CPU=IA64 PROCESSOR_ARCHITECTURE=x86 ;;
    esac
    OS=Windows_NT
    export NTSDK NTCPU NTRUN_DRIVE_C NTRUN_PATH NTRUN_DLLPATH INCLUDE LIB CPU PROCESSOR_ARCHITECTURE OS
    if [ -n "$NTRUN_DRIVE_D" ]; then export NTRUN_DRIVE_D; fi
    if [ -n "$CL" ]; then export CL; fi
    if [ -n "$APPVER" ]; then export APPVER TARGETOS; fi
    # a tool of the SDK, by its name: "ntsdk_run cl -c x.c"
    return 0
}

ntsdk_run() { "$NTRUN" "$@"; }
