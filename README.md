# ntrun

Runs Windows NT console programs made for MIPS, Alpha AXP, PowerPC or x86 on macOS or Linux.
Windows 10 and later is supported as a host too, but that is experimental (see "Build on Windows").
Its job: run the original build tools of the NT SDKs and of Visual C++
(cl, link, lib, nmake, rc, mc, midl ...) without a VM and without Wine,
and run the console programs that these tools build.

The Docker image [ntsdk](https://github.com/tenox7/ntsdk) has ntrun with these tools for
Windows NT 3.1, 3.5 and 4.0 and the four CPUs.

ntrun runs 32-bit programs only. The tools that make programs for 64-bit Alpha (AXP64) are
32-bit Alpha programs, and the cross compiler for Itanium (IA-64) is an x86 program, so they run.
The programs that they make do not run.

## How it works

- A CPU interpreter runs the code of the program (`cpu_mips.c`, `cpu_alpha.c`, `cpu_ppc.c`, `cpu_x86.c`).
  x86 is the 80486 instruction set with the x87 and the CMOV instructions. The x87 is done with
  integer arithmetic only (80-bit registers, each precision and rounding mode, exceptions),
  so each host gives the same bits.
- The Windows functions (KERNEL32, and some of NTDLL, USER32, ADVAPI32, RPCRT4, IMAGEHLP, OLE32) run on the host (`ntrun.c`).
- DLLs that are not part of Windows (MSVCRT40.DLL, MSPDB41.DLL, CRTDLL.DLL ...) are loaded from files
  and run in the interpreter too.
- Windows exceptions work as on Windows NT (`seh.c`): function tables and unwind on the RISC CPUs,
  the handler list on x86; `__try`, C++ exceptions, `setjmp`/`longjmp`.
- The start function of a DLL runs after those of the DLLs that it imports from, and each DLL
  gets the notice that the program ends (the C runtime DLL of 2005 writes the last output then).
- A program that starts another program (cl starts c1, c2 and link) starts a new ntrun.
- Each thread of the program is a host thread. Only one of them runs at a time.
- The command processor is part of ntrun (`cmd.c`): `cmd /c` lines of makefiles, batch files,
  `if`, `for`, `&&`, `|`, redirection, `copy`, `del`, `md` and so on.
  The empty file `cmd.exe` next to the binary stands for it (nmake looks for the file of `COMSPEC`).

## Build

    make

or

    cc -O2 -o ntrun ntrun.c cmd.c seh.c cpu_mips.c cpu_alpha.c cpu_ppc.c cpu_x86.c -lm -lpthread

- 64-bit hosts only (the program gets a 4 GB address range).
- Linux: `make LDFLAGS=-static` gives a binary that needs no libraries.
- Keep the empty `cmd.exe` next to the binary.
- `x86args.h` is made by `tools/x86args.pl` from the import libraries of the SDKs (see the script).
  Make it again when you add a Windows function to `ntrun.c`.

## Build on Windows (experimental)

ntrun builds as a native Windows program with MSYS2, in the UCRT64 or the MINGW64 shell (gcc), with no
Cygwin layer:

    pacman -S make mingw-w64-ucrt-x86_64-gcc      # once, in the MSYS2 UCRT64 shell
    pacman -S make mingw-w64-x86_64-gcc           # or in the MINGW64 shell
    make CC=gcc

UCRT64 is the preferred one. The C library of MINGW64 (`msvcrt.dll`) has an `fma` that is not exactly rounded,
so a MINGW64 build uses its own (`softfma.h`) for the multiply-add of the PowerPC.

- The Makefile links the program statically on Windows, so that a program that ntrun starts
  does not need the DLLs of MSYS2 on its `PATH`.
- The program has a manifest (`ntrun.manifest`, put in by `windres`) that makes it aware of paths longer than
  259 characters. Windows uses it only when `LongPathsEnabled` is 1 in the registry
  (`HKLM\SYSTEM\CurrentControlSet\Control\FileSystem`); otherwise the limit of Windows stays 259.
  A program of Windows cannot start in a folder with a path longer than 259 characters.
- Run it from an MSYS2 shell with `MSYS2_ARG_CONV_EXCL="*"`: otherwise MSYS changes an argument
  like `/c` into a path.
- `NTRUN_DLLPATH` has `;` between the folders on Windows (a drive letter has `:`).
- Host paths have a drive: `C:/dir`. If you give a folder for a drive (`NTRUN_DRIVE_C`), the program sees
  the host's `C:\dir` as `Z:\c\dir`; `Z:\dir` is below the root of the system drive.
- Memory is committed as the program allocates it. A file view that the program can write is
  a real view of the file when the view is made with no address and the offset is a multiple of
  64 KB; a view that cannot be mapped is a copy that is written back to the file when the program
  flushes, reads or writes the file, closes it or ends.
- Windows is a host of the same ntrun: the tests below run on it too.

## Use

    ntrun [-a] [-t] PROGRAM [arguments]

- PROGRAM is a file (`tools/BIN/CL.EXE`) or a name (`cl`) that ntrun finds in the folders of `NTRUN_PATH`.
  Use the name form for tools that look at their own name (the MIPS `cc`).
- `ntrun cmd /c "line"` runs a command line. `ntrun cmd` reads command lines from the standard input.
- `-a` prints each Windows call (`-a -a`: also before the call). `-t` prints each instruction.
- The exit code is the exit code of the program.

Environment variables:

| Variable | Use |
|---|---|
| `NTRUN_PATH` | The `PATH` of the program: Windows folders with `;` between them. Default: the folder of the program. |
| `NTRUN_DLLPATH` | Host folders with DLLs, with `:` between them (`;` on Windows). |
| `NTRUN_DRIVE_C` (any letter) | A host folder for that drive: `NTRUN_DRIVE_C=/opt/nt` makes `C:\MSTOOLS` the folder `/opt/nt/MSTOOLS`. |
| `NTRUN_TRACE` | 1 or 2: the same as `-a` or `-a -a`, also for the programs that the program starts. |
| `INCLUDE`, `LIB`, `TMP` ... | Go to the program as they are. Give Windows paths. |

The program gets all the other variables of the host too, but for these:

- `LANG`, `LANGUAGE`, `LC_*`, `SHELL`, `SHLVL`, `PWD`, `OLDPWD`, `_`, `TERM`: variables of the Unix shell.
  NMAKE makes a macro of each variable, and some makefiles have a macro `LANG` of their own.
- `MAKEFLAGS` and the other variables of GNU make, when GNU make starts ntrun (NMAKE reads `MAKEFLAGS` too).
- `TMPDIR` goes to the program as a Windows path.

Paths:

- A drive with no `NTRUN_DRIVE_` variable is the root of the host: `Z:\tmp` is `/tmp`.
- File names are found in any case (`STDIO.H` finds `stdio.h`).
- Keep the Windows paths short. Some old tools have small buffers: `rc` of the NT 3.5 SDK
  stops when the include path list is longer than about 500 characters. Use a drive variable for that.
- `TMP` and `TEMP` are the temporary folder of the host if you do not set them.
- A new file in the root folder of a drive that is the host root goes to the temporary folder
  (the Microsoft C library puts `tmpfile()` files in the root folder).

Example (Visual C++ 4.1 for MIPS in /opt/vc41/mips, CRTDLL.DLL of NT for MIPS in /opt/ntsys/mips):

    export NTRUN_DRIVE_C=/opt/vc41/mips
    export NTRUN_PATH='C:\BIN' NTRUN_DLLPATH=/opt/vc41/mips/REDIST:/opt/ntsys/mips
    export INCLUDE='C:\INCLUDE;C:\MFC\INCLUDE' LIB='C:\LIB;C:\MFC\LIB'
    ntrun nmake CPU=MIPS
    ntrun cl -MD hello.c && ntrun hello.exe

Example (the x86 tools of the `nt40sdk` folder of the ntsdk repository, in that folder;
CRTDLL.DLL of NT for x86 in /opt/ntsys/x86):

    export NTRUN_DRIVE_C=$PWD/nt40sdk
    export NTRUN_PATH='C:\mstools\bin;C:\msdev\bin'
    export NTRUN_DLLPATH=$PWD/nt40sdk/msdev/redist:/opt/ntsys/x86
    export INCLUDE='C:\mstools\include;C:\msdev\include;C:\msdev\mfc\include'
    export LIB='C:\mstools\lib;C:\msdev\lib;C:\msdev\mfc\lib' CPU=i386
    ntrun nmake

DLLs of Windows that ntrun does not have itself: some tools need `CRTDLL.DLL` (cvtres, the
NT 3.1 SDK tools) or `MSVCRT.DLL` (the NT 4.0 SDK tools) of Windows NT for their CPU.
Copy them from the `system32` folder of a Windows NT system, or from its CD
(`I386\CRTDLL.DL_`, `MIPS\MSVCRT.DL_` ...; 7-Zip expands them), to a folder of `NTRUN_DLLPATH`.

## Tests

    tests/run.sh [TOOLS [mips alpha ppc x86 axp64 ia64]]
    make test TOOLS=/folder/with/the/tool/sets

TOOLS is a folder with Visual C++ 4.x for each CPU, in the layout of the ntsdk image
(`nt40/CPU/msdev`, `sys/CPU`; see the script). The default is `/opt`, the folder in that image.
So the tests run in the image, with the ntrun of the image or with a new one:

    docker run --rm -v "$PWD":/ntrun -e NTRUN=ntrun tenox7/ntsdk sh /ntrun/tests/run.sh
    docker run --rm -v "$PWD":/ntrun tenox7/ntsdk sh /ntrun/tests/run.sh      # ./ntrun, a static Linux binary

On macOS, with the ntsdk repository next to this one: `tests/run.sh ../ntsdk/tools`.
On Windows (MSYS2), the same, with `MSYS2_ARG_CONV_EXCL="*"` set: `tests/run.sh ../ntsdk/tools`.

Builds the test programs with Visual C++ 4.x under ntrun and runs them under ntrun:
exceptions (`seh`, `jmpex`, `eh`), thread-local data (`tls`), threads (`thread`),
arithmetic (`num`, `fpx`), the math functions of the C library (`math`), the x87 (`x87`, for x86 only),
the MD5 and SHA-1 sums of the system (`hash`, against the values that `md5` and `shasum` give),
the tools (`tools`: mc, rc, lib, a DLL), a pipe from a child process (`popen`: `_popen` and the pipe of `CreatePipe`),
file views that agree with the file (`mapview`) and the command processor (`cmdtest.bat`, `drive`, and `pipe`:
a pipeline whose second command ends without reading must not hang).
`axp64` builds a C++ program, a DLL and a program with resources with the tools for 64-bit Alpha
(`nt50/axp64` of TOOLS) and reads the headers of the files: ntrun cannot run them.
`ia64` does the same with the cross compiler for Itanium (`nt52/ia64` of TOOLS).
The `*.out` files are what the same programs printed on real Windows NT 3.51
(MIPS, Alpha, PowerPC and x86 VMs; `cmdtest` on CMD.EXE of NT 3.51). `popen.out` and `mapview.out` are what the
programs must print by the rules of Windows; they were not made on a real NT system.
The CPUs are not the same in floating point, and each has its own C library: `num`, `fpx` and `math`
have one file for each CPU. `math` prints a sum of the bits of 12000 results (`exp`, `log`, `pow`,
`atan`, `sinh`, `fmod` ...): on each of the four CPUs it is the same as on real Windows NT.

`x87` does the x87 instructions for 9300 sets of operands, in each precision and rounding mode,
and prints a sum of the result bits for each instruction. The NT x86 VM is a QEMU VM, and QEMU
is not a real x87 in all things. Where the two are not the same, ntrun does what the Intel
manual says, and `x87` does not test it:

- SIN, COS and TAN: QEMU calculates them with 53 bits. ntrun gives the correctly rounded 64 bits.
- The precision (24 or 53 bits) is for `+ - * /` and the square root only. QEMU uses it for
  loads, the remainder and the round-to-integer instruction too.
- FYL2X with an operand below the smallest normal number: QEMU gives two times the result.
- QEMU does not see a push to a full register stack or the use of an empty register.
- QEMU does not set C1 (the rounding made the result larger) or, for FPREM1, the bits of the quotient.

## Tested on 2026-11-21 (macOS arm64, Linux arm64 and Linux x86-64: 45 of 45 tests)

| Tool set | CPUs | Tested |
|---|---|---|
| Visual C++ 4.1 | MIPS, Alpha, PowerPC | cl (C, C++, exceptions, -MD, -Zi, -Z7, -FR, precompiled headers), link (also -debug, incremental, DLL), lib, nmake, rc, mc, midl, cvtres, cvpack, bscmake, dumpbin, editbin, uuidgen, asaxp; MFC programs (static and DLL) |
| Visual C++ 2.0 | MIPS, x86 | the same list |
| Visual C++ 4.2 with the NT 4.0 SDK | x86 | the same list, MFC programs |
| Visual C++ 6.0 | Alpha | cl, link, lib, nmake, rc, midl, asaxp, bscmake, dumpbin, editbin |
| NT 3.1 SDK (1993, 1994) | MIPS, Alpha, x86 | cl, cl386, mcl, claxp, cc + mip2coff (old MIPS compiler), acc, link32, nmake, rc + cvtres, mc, midl, asaxp, rebase, compress |
| NT 3.5 SDK | Alpha, x86 (with Visual C++ 2.0) | cl, link, nmake, rc, mc, midl, asaxp; all the SDK samples with their own makefiles (Alpha: 347 programs, x86: 349) |
| NT 4.0 SDK tools | MIPS, Alpha, PowerPC | mc, rc, midl 3.00 (with the Visual C++ 4.1 compiler; they need MSVCRT.DLL of NT 4.0) |
| Platform SDK for Windows 2000 RC2, the tools for 64-bit Alpha | Alpha (the programs they make are for AXP64) | cl 13.00 (C, C++, exceptions, -MD, -Zi, -Z7), link 6.20 (also -debug, DLL), lib, rc, mc, midl -win64, cvtres, asaxp, dumpbin, editbin, nmake 6.00 of Visual C++ 6.0; MFC programs (static and DLL) |
| Platform SDK for Windows Server 2003 SP1, the cross compiler for Itanium | x86 (the programs they make are for IA-64) | cl 14.00 (C, C++, exceptions, -MD, -Z7), link 8.00 (also DLL, -dump, -edit), lib, nmake 7.00, rc, mc, midl -win64, cvtres, ias; MFC programs (static and DLL) |

- A full `nmake` build of a small program (ntrshd) gives the same file as a build on Windows NT, byte for byte,
  but for the time stamps: Visual C++ 4.1 on the three RISC CPUs, the NT 3.1 SDK for MIPS, Alpha and x86,
  Visual C++ 2.0 and 4.2 for x86 (the link on a real NT 3.51 x86 machine).
- Itanium: 12 object files (C, C++, MFC, a 500 KB program with `/Ox`, the assembler `ias`) and a linked
  C program are the same files as from the same tools under Wine 8, but for the time stamps. There is no
  build from a real Windows system to compare with. The linker of this SDK writes unused memory into some
  bytes of a C++ program, so such a program is not the same file on two systems. `.pdb` files do not work:
  `cl -Zi` and `link -debug` need a server program (`mspdbsrv.exe`) that talks RPC.
- AXP64: two programs (a console program, and a 628 KB program with windows, compiled with `/Ox`) are the
  same files as from a build with the same tools on Windows for Alpha, byte for byte, but for the time stamps.
- A link with the x86 `link` of Visual C++ 2.0 or 4.2 under Wine does not give the same file as on Windows NT:
  the imports are in another order (about 380 bytes of the program differ). Under ntrun the file is the same.
- Object files from the x86 compilers are the same as under Wine, also for floating point
  constants that the compiler calculates.
- The x86 `nt35sdk` folder of the ntsdk repository has no `IDL` folder of the SDK (`unknwn.idl` ...).
  The samples `OLE\MFRACT` and `RPC\OBJECT` need it in `INCLUDE`.
- The programs built this way ran on the NT 3.51 MIPS, Alpha, PowerPC and x86 VMs.
- x86: `sin` and `cos` (4 % of the results) and `tan` (44 %) of the C library do not give the same
  last bit or two as on the NT x86 VM: ntrun has the correctly rounded result (checked with bc),
  the VM does not (see Tests). All the other math functions give the same bits.

## Not done

- Programs with windows (USER32 and GDI32 are not there). They build; they do not run here.
- `mktyplib` (needs OLE) and `makehm` (needs MFC with windows).
- The help compilers `hc30` and `hc31`: they are 16-bit DOS programs.
- Tools for a running Windows NT system (`pstat`, `top`, `sc`, the kernel debuggers).
- Sockets, named pipes, the registry (it is empty), services.
- x86: MMX and SSE.
- The command processor: `set /a`, `for /f`, `start`.
