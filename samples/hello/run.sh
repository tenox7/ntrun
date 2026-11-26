#!/bin/sh
# run.sh [SDK-CPU ...]: build the test program (hello.c, hello.rc, makefile) with nmake for each
# SDK and CPU of the ntsdk repository (../ntsdk next to this project, or NTSDK_ROOT) and run it with ntrun.
# No Docker: ntsdk-env.sh sets the environment that the ntsdk script of the image sets.
# The program prints the CPU type of its own file and the version of its compiler.
# ntrun cannot run a program for 64-bit Alpha (axp64) or Itanium (ia64): there the test reads
# the header of the program with the dump of the linker.
#
# On MSYS2 run it with MSYS2_ARG_CONV_EXCL="*" (the shell must not change /c into C:/).
# KEEP=1 keeps the work folder. The work folder is $WORK (default: /tmp/ntsdk-tests).

here=$(cd "$(dirname "$0")" && pwd)
. "$here/ntsdk-env.sh"
work=${WORK:-/tmp/ntsdk-tests}
fail=0
total=0

ok() { total=$((total + 1)); echo "  ok    $1"; }
bad() { total=$((total + 1)); fail=$((fail + 1)); echo "  FAIL  $1"; }

list="nt31-x86:014c:800 nt31-mips:0166:800 nt31-alpha:0184:800
nt35-x86:014c:900 nt35-mips:0166:900 nt35-alpha:0184:900
nt40-x86:014c:1020 nt40-mips:0166:1010 nt40-alpha:0184:1010 nt40-ppc:01f0:1010
nt50-axp64:0284:1300 nt52-ia64:0200:1400"
if [ $# -gt 0 ]; then
    want="$*"
else
    want=
fi

echo "== samples/hello"
for item in $list; do
    sc=${item%%:*}
    rest=${item#*:}
    machine=${rest%%:*}
    compiler=${rest#*:}
    sdk=${sc%%-*}
    cpu=${sc#*-}
    if [ -n "$want" ]; then
        case " $want " in *" $sc "*) ;; *) continue ;; esac
    fi
    d=$work/$sc
    rm -rf "$d"
    mkdir -p "$d"
    cp "$here"/hello.c "$here"/hello.rc "$here"/makefile "$d"/
    cd "$d" || exit 1
    ntsdk_env $sdk $cpu || { bad "$sc"; continue; }
    TMP=$(ntsdk_win "$d")
    export TMP
    # nt52: dumpbin and editbin are "link -dump" and "link -edit"
    "$NTRUN" nmake -nologo > nmake.txt 2>&1
    case $cpu in
    axp64 | ia64)
        if [ $cpu = axp64 ]; then name=Alpha64; else name=IA64; fi
        if [ $sdk = nt52 ]; then
            "$NTRUN" link -dump -headers hello.exe > hello.txt 2> hello.err
        else
            "$NTRUN" dumpbin -headers hello.exe > hello.txt 2> hello.err
        fi
        if grep -q " ${machine#0} machine ($name)" hello.txt && grep -q "PE32+" hello.txt && grep -q "\.rsrc" hello.txt; then
            ok "$sc (built, not run: a 64-bit program)"
        else
            bad "$sc (see nmake.txt, hello.txt and hello.err in $d)"
        fi
        continue
        ;;
    esac
    "$NTRUN" hello.exe > hello.txt 2> hello.err
    printf 'hello: machine %s, compiler %s\n42 7.485471 text from the resources\n' $machine $compiler > expected.txt
    if tr -d '\r' < hello.txt | cmp -s - expected.txt; then
        ok "$sc"
    else
        bad "$sc (see nmake.txt, hello.txt and hello.err in $d)"
    fi
done

echo "$((total - fail)) of $total tests passed"
cd "$here" || exit 1
[ -n "$KEEP" ] || [ $fail -ne 0 ] || rm -rf "$work"
[ $fail -eq 0 ]
