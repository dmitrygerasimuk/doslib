
#include <stdio.h>
#include <conio.h> /* this is where Open Watcom hides the outp() etc. functions */
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <fcntl.h>
#include <math.h>
#include <dos.h>

#include <hw/cpu/cpu.h>
#include <hw/dos/dos.h>
#include <hw/vga/vga.h>
#include <hw/8254/8254.h>
#include <hw/8259/8259.h>
#include <hw/vga/vgatty.h>
#include <hw/dos/doswin.h>

#define __ASM_SETCHARMAXLINE(h)                         \
        __asm {                                         \
            __asm   mov         dx,3D4h                 \
            __asm   mov         al,9h                   \
            __asm   out         dx,al                   \
            __asm   inc         dx                      \
            __asm   mov         al,h                    \
            __asm   out         dx,al                   \
        }

#define __ASM_SETMODESEL(x)                             \
        __asm {                                         \
            __asm   mov         dx,3D8h                 \
            __asm   mov         al,x                    \
            __asm   out         dx,al                   \
        }

static inline void __ASM_WAITFORVSYNC(void) {
        __asm {
                ; wait for vsync
                mov         dx,3DAh
wvsync:         in          al,dx
                test        al,8
                jz          wvsync

                ; wait for (vsync | blank) end
wvsyncend:      in          al,dx
                test        al,9        ; 8 (vsync) | 1 (blank)
                jnz         wvsyncend
        }
}

static inline void __ASM_WAITLINE_INNER(void) {
        /* caller should have loaded DX = 3DAh */
        __asm {
                ; wait for hblank
wvsync:         in          al,dx
                test        al,1        ; 1 blank
                jz          wvsync

                ; wait for hblank end
wvsyncend:      in          al,dx
                test        al,1        ; 1 blank
                jnz         wvsyncend
        }
}

static inline void __ASM_WAITLINE(void) {
        __asm mov dx,3DAh

        __ASM_WAITLINE_INNER();
}

#define __ASM_WAITLINES(name,c)                             \
        __asm {                                             \
            __asm   mov         dx,3DAh                     \
            __asm   mov         cx,c                        \
            __asm p1##name:                                 \
            __asm   in          al,dx                       \
            __asm   test        al,1                        \
            __asm   jz          p1##name                    \
            __asm p2##name:                                 \
            __asm   in          al,dx                       \
            __asm   test        al,1                        \
            __asm   jnz         p2##name                    \
            __asm   loop        p1##name                    \
        }

int main() {
    /* NTS: In this demo, the row start and height must be a multiple of 8.
     *      Because 40x25 text mode, which has a rowheight of 8, and 640x200 2-color mode, with a rowheight of 2.
     *      We don't want to cause any condition where the 6845 row counter runs off beyond 8 (and up through 31 back around to 0) */
    unsigned short row=8; /* cannot be <= 1, multiple of 8 */
    unsigned short rowheight=8; /* RECOMMENDED!! Multiple of 8 */
    unsigned short rowa=8;
    unsigned short rowad=0;
    unsigned short vtadj = (rowheight / 2) - 1; /* adjust to vtotal because text rows become graphics */
    unsigned short row1;

    printf("WARNING: Make sure you run this on CGA hardware!\n");
    printf("Hit ENTER to continue, any other key to exit\n");

    if (getch() != 13) return 1;

    __asm {
            mov     ax,1
            int     10h
    }

    printf("40x25 mode select raster fx test.\n");
    printf("Hit ESC at any time to stop.\n");

    /* we also need to fill the remainder of RAM with something */
    printf("\n");
    {
        unsigned int x,y;

        for (y=0;y < 16;y++) {
            for (x=0;x < 39;x++) {
                printf("%c",(x+y)|0x20);
            }

            printf("\n");
        }
    }

    /* the 640x200 portion isn't going to show anything unless we set port 3D9h to set foreground to white.
     * this will also mean the border will be white for text mode, but, meh */
    outp(0x3D9,0x0F);

    _cli();
    do {
        if (inp(0x64) & 1) { // NTS: This is clever because on IBM PC hardware port 64h is an alias of 60h.
            unsigned char c = inp(0x60); // chances are the scancode has bit 0 set.
            // remember that on AT hardware, port 64h bit 0 is set when a keyboard scancode has come in.
            // on PC/XT hardware, 64h is an alias of port 60h, which returns the latest scancode transmitted
            // by the keyboard. to keep the XT keyboard going, we have to set bit 7 of port 61h to clear
            // and restart the scanning process or else we won't get any more scan codes. setting bit 7
            // on AT hardware usually has no effect. We also have to ack IRQ 1 to make sure keyboard
            // interrupts work when we're done with the program.
            //
            // by reading port 64h and 60h in this way we can break out of the loop reliably when the user
            // hits a key on the keyboard, regardless whether the keyboard controller is AT or PC/XT hardware.

            if (c != 0 && (c & 0x80) == 0)
                break;
        }

        {
            unsigned char x = inp(0x61) & 0x7F;
            outp(0x61,x | 0x80);
            outp(0x61,x);

            outp(0x20,P8259_OCW2_SPECIFIC_EOI | 1); // IRQ1
        }

        {
            /* because we're changing row height, we need to adjust vtotal/vdisplay to keep normal timing */
//            unsigned int n_rows = ((0x1F/*total*/ + 1) * 8) + 6/*adjust*/; /* = 262 lines aka one normal NTSC field */
            unsigned int rc = 0x1F + 1 + vtadj;

            /* program the new vtotal/vadjust to make the video timing stable */
            outp(0x3D4,0x04); /* vtotal */
            outp(0x3D5,rc-1);
            outp(0x3D4,0x07); /* vsyncpos */
            outp(0x3D5,rc-1-3);
            outp(0x3D4,0x06); /* vdisplay */
            outp(0x3D5,rc-1-3-3);
        }

        /* begin */
        __asm {
                push        cx
                push        dx
        }

        row1 = row - 1;

        __ASM_SETCHARMAXLINE(7);        /* 8 lines */
        __ASM_WAITFORVSYNC();

        __ASM_SETMODESEL(0x28);         /* 40x25 text */
        __ASM_WAITLINES(waitv1,row);

        __ASM_SETMODESEL(0x1A);         /* 640x200 2-color graphics with colorburst enabled */
        __ASM_SETCHARMAXLINE(1);        /* 2 lines */
        __ASM_WAITLINES(waitv2,rowheight);

        __ASM_SETMODESEL(0x28);         /* 40x25 text */
        __ASM_SETCHARMAXLINE(7);        /* 8 lines */

        __asm {
                pop         dx
                pop         cx
        }

        if (++rowad == 60) {/* assume 60Hz */
            rowad = 0;
            row += rowa;

            if ((row+rowheight) >= 190)
                rowa = -8;
            else if (row <= 9)
                rowa = 8;
        }
    } while(1);

    {
        unsigned char x = inp(0x61) & 0x7F;
        outp(0x61,x | 0x80);
        outp(0x61,x);

        outp(0x20,P8259_OCW2_SPECIFIC_EOI | 1); // IRQ1
    }

    _sti();
    while (kbhit()) getch();

    __asm {
            mov     ax,3
            int     10h
    }

    return 0;
}

