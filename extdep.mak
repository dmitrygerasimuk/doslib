# makefile for external projects to use this library.
# external projects should !include this file after
# setting makefile variable DOSLIBDIR

!ifndef DOSLIBDIR
! error DOSLIBDIR not defined
!endif

DOSLIB_CINCLUDE=-i$(DOSLIBDIR)
DOSLIB_LINCLUDE=
# note: -0 means to target the 8086/8088
DOSLIB_DEFS_16COMMON=-DTARGET_MSDOS=16 -DMSDOS=1 -DTARGET86=86 -0

# note: -3 means to target the 386+
DOSLIB_DEFS_32COMMON=-DTARGET_MSDOS=32 -DMSDOS=1 -DTARGET86=386 -3

DOSLIB_CFLAGS_DOS16C=$(DOSLIB_CINCLUDE) $(DOSLIB_DEFS_16COMMON) -DMMODE=c -mc -bt=dos
DOSLIB_LDFLAGS_DOS16C=$(DOSLIB_LINCLUDE) system dos

DOSLIB_CFLAGS_DOS16S=$(DOSLIB_CINCLUDE) $(DOSLIB_DEFS_16COMMON) -DMMODE=s -ms -bt=dos
DOSLIB_LDFLAGS_DOS16S=$(DOSLIB_LINCLUDE) system dos

DOSLIB_CFLAGS_DOS16M=$(DOSLIB_CINCLUDE) $(DOSLIB_DEFS_16COMMON) -DMMODE=m -mm -bt=dos
DOSLIB_LDFLAGS_DOS16M=$(DOSLIB_LINCLUDE) system dos

DOSLIB_CFLAGS_DOS16L=$(DOSLIB_CINCLUDE) $(DOSLIB_DEFS_16COMMON) -DMMODE=l -ml -bt=dos
DOSLIB_LDFLAGS_DOS16L=$(DOSLIB_LINCLUDE) system dos

DOSLIB_CFLAGS_DOS16H=$(DOSLIB_CINCLUDE) $(DOSLIB_DEFS_16COMMON) -DMMODE=h -mh -bt=dos
DOSLIB_LDFLAGS_DOS16H=$(DOSLIB_LINCLUDE) system dos

DOSLIB_CFLAGS_DOS32=$(DOSLIB_CINCLUDE) $(DOSLIB_DEFS_32COMMON) -DMMODE=f -mf -bt=dos
DOSLIB_LDFLAGS_DOS32=$(DOSLIB_LINCLUDE) system dos4g

# NOTES: It is recommended but not required that you use the -zu flag if you will call subroutines from an interrupt handler!
