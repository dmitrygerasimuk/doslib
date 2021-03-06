
; NTS: If we code 'push ax' and 'popf' for the 16-bit tests in 32-bit protected mode we will screw up the stack pointer and crash
;      so we avoid duplicate code by defining 'native' pushf/popf functions and 'result_reg' to ax or eax depending on CPU mode
%if TARGET_MSDOS == 32
 %define point_s esi
 %define result_reg eax
 %define pushan pushad
 %define popan popad
 %define pushfn pushfd
 %define popfn popfd
%else
 %define point_s si
 %define result_reg ax
 %define pushan pusha
 %define popan popa
 %define pushfn pushf
 %define popfn popf
%endif

