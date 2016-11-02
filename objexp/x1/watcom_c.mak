
HERE = $+$(%cwd)$-

OBJS =     cmm.obj

CC       = wcc
CFLAGS   = -zq -ml -s -bt=dos -oilrtm -fr=nul -wx -0 -fo=.obj -q -zu -zdf -zff -zgf -zc -fpi87 -i../.. -dTARGET_MSDOS=16 -dMSDOS=1

all: final.exe test.exe

header.asm: final.def
	./clsggen.pl --def $[@ --asm $@ --enum final.h

header.obj: header.asm
	nasm -o $@ -f obj $(NASMFLAGS) $[@

.C.OBJ:
	$(CC) $(CFLAGS) $[@

../../hw/dos/dos86l/dos.lib:
	@cd ../../hw/dos
	@./make.sh build lib
	@cd $(HERE)

test.exe: test.obj ../../hw/dos/dos86l/dos.lib
	%write tmp.cmd option quiet system dos
	%write tmp.cmd library ../../hw/dos/dos86l/dos.lib
	%write tmp.cmd file test.obj
	%write tmp.cmd name test.exe
	%write tmp.cmd option map=test.map
	@wlink @tmp.cmd

cmm.obj: cmm.c
	$(CC) $(CFLAGS) -ms -zl $[@

final.exe: cmm.obj header.obj
	%write tmp.cmd option quiet system dos
	%write tmp.cmd file header.obj
	%write tmp.cmd file cmm.obj
	%write tmp.cmd name final.exe
	%write tmp.cmd option map=final.map
	@wlink @tmp.cmd

clean: .SYMBOLIC
          del *.obj
          @echo Cleaning done