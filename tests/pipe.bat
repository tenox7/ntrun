@echo off
rem pipe.bat: the first command of a pipeline writes more than the pipe holds (64 KB) and the
rem second command ends without reading. The first command must get an error and end: it must
rem not wait for ever on a pipe that nobody reads (the pipe is not inherited by the first command).
echo 1 start
echo xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx> big1.tmp
copy big1.tmp+big1.tmp big2.tmp > nul
copy big2.tmp+big2.tmp big1.tmp > nul
copy big1.tmp+big1.tmp big2.tmp > nul
copy big2.tmp+big2.tmp big1.tmp > nul
copy big1.tmp+big1.tmp big2.tmp > nul
copy big2.tmp+big2.tmp big1.tmp > nul
copy big1.tmp+big1.tmp big2.tmp > nul
copy big2.tmp+big2.tmp big1.tmp > nul
copy big1.tmp+big1.tmp big2.tmp > nul
copy big2.tmp+big2.tmp big1.tmp > nul
copy big1.tmp+big1.tmp big2.tmp > nul
rem big2.tmp has 2048 lines of 68 bytes: 139264 bytes
type big2.tmp | cmd /c exit 0
echo 2 pipeline ended
del big1.tmp
del big2.tmp
