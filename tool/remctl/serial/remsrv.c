
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <conio.h>
#include <fcntl.h>
#include <dos.h>

#include <hw/cpu/cpu.h>
#include <hw/dos/dos.h>
#include <hw/dos/doswin.h>
#include <hw/8254/8254.h>
#include <hw/8259/8259.h>
#include <hw/8250/8250.h>
#include <hw/8250/8250pnp.h>
#include <hw/isapnp/isapnp.h>
#include <hw/flatreal/flatreal.h>

#include "proto.h"

static struct info_8250 *uart = NULL;
static unsigned long baud_rate = 115200;
static unsigned char use_interrupts = 1;
static unsigned char halt_system = 0;
static unsigned char stop_bits = 1;

static struct remctl_serial_packet      cur_pkt_in = {0};
static unsigned char                    cur_pkt_in_write = 0;       // from 0 to < sizeof(cur_pkt_in)
static unsigned char                    cur_pkt_in_seq = 0xFF;

static struct remctl_serial_packet      cur_pkt_out = {0};
static unsigned char                    cur_pkt_out_write = 0;      // from 0 to < sizeof(cur_pkt_out)
static unsigned char                    cur_pkt_out_seq = 0xFF;

void process_input(void);
void process_output(void);

int uart_waiting_read = 0;
int uart_waiting_write = 0;

/* NOTE: You're supposed to call this function with interrupts disabled,
 *       or from within an interrupt handler in response to the UART's IRQ signal. */
static void irq_uart_handle_iir(struct info_8250 *uart) {
    unsigned char reason,c,patience = 8,act_reason = 0;

    /* why the interrupt? */
    /* NOTE: we loop a maximum of 8 times in case the UART we're talking to happens
     *       to be some cheap knockoff chipset that never clears the IIR register
     *       when all events are read */
    /* if there was actually an interrupt, then handle it. loop until all interrupt conditions are depleted */
    while (((reason=inp(uart->port+PORT_8250_IIR)&7)&1) == 0) {
        reason >>= 1;

        if (reason == 3) { /* line status */
            c = inp(uart->port+PORT_8250_LSR);
            /* do what you will with this info */
        }
        else if (reason == 0) { /* modem status */
            c = inp(uart->port+PORT_8250_MSR);
            /* do what you will with this info */
        }
        else {
            // 2 = data avail
            // 1 = transmit empty
            act_reason = 1;
        }

        if (--patience == 0)
            break;
    }

    if (act_reason) {
        uart_waiting_read = 0;
        uart_waiting_write = 0;
        process_input();
        process_output();
    }
}

void halt_system_loop(void) {
    /* interrupts at this point should be disabled */
    do {
        uart_waiting_read = 0;
        uart_waiting_write = 0;
        process_input();
        process_output();
    } while (halt_system);
}

static void (interrupt *old_timer_irq)() = NULL;
static void interrupt timer_irq() {
    if (use_interrupts && uart->irq != -1) {
        unsigned char fix_irq = 0;
        unsigned int i;

        if (uart_8250_can_read(uart)) {
            if ((++uart_waiting_read) >= 9) {
                uart_waiting_read = 0;
                fix_irq = 1;
            }
        }
        if (uart_8250_can_write(uart)) {
            if ((++uart_waiting_write) >= 9) {
                uart_waiting_write = 0;
                fix_irq = 1;
            }
        }

        if (fix_irq) {
            _cli();
            p8259_mask(uart->irq);

            if (uart->irq >= 8) p8259_OCW2(8,P8259_OCW2_SPECIFIC_EOI | (uart->irq&7));
            p8259_OCW2(0,P8259_OCW2_SPECIFIC_EOI | (uart->irq&7));

            uart_8250_enable_interrupt(uart,0x0);
            uart_8250_set_MCR(uart,3);      /* turn on RTS and DTS */
            uart_8250_set_line_control(uart,UART_8250_LCR_8BIT | UART_8250_LCR_PARITY | (stop_bits == 2 ? UART_8250_LCR_TWO_STOP_BITS : 0)); /* 8 bit 1 stop bit odd parity */
            uart_8250_set_baudrate(uart,uart_8250_baud_to_divisor(uart,baud_rate));

            for (i=0;i < 256 && (inp(uart->port+PORT_8250_IIR) & 1) == 0;i++);

            uart_8250_enable_interrupt(uart,0xF);
            process_input();
            process_output();
            p8259_unmask(uart->irq);
        }
    }

    /* halt here if instructed */
    if (halt_system) halt_system_loop();

    old_timer_irq();
}

static void (interrupt *old_irq)() = NULL;
static void interrupt uart_irq() {
    /* clear interrupts, just in case. NTS: the nature of interrupt handlers
     * on the x86 platform (IF in EFLAGS) ensures interrupts will be reenabled on exit */
    _cli();
    irq_uart_handle_iir(uart);

    /* ack PIC */
    if (uart->irq >= 8) p8259_OCW2(8,P8259_OCW2_NON_SPECIFIC_EOI);
    p8259_OCW2(0,P8259_OCW2_NON_SPECIFIC_EOI);

    /* halt here if instructed */
    if (halt_system) halt_system_loop();
}

void begin_output_packet(const unsigned char type) {
    cur_pkt_out.hdr.mark = REMCTL_SERIAL_MARK;
    cur_pkt_out.hdr.length = 0;
    cur_pkt_out.hdr.sequence = cur_pkt_out_seq;
    cur_pkt_out.hdr.type = type;
    cur_pkt_out.hdr.chksum = 0;

    if (cur_pkt_out_seq == 0xFF)
        cur_pkt_out_seq = 0;
    else
        cur_pkt_out_seq = (cur_pkt_out_seq + 1) & 0x7F;
}

void end_output_packet(void) {
    unsigned char sum = 0;
    unsigned int i;

    for (i=0;i < cur_pkt_out.hdr.length;i++)
        sum += cur_pkt_out.data[i];

    cur_pkt_out.hdr.chksum = 0x100 - sum;
}

unsigned char is_v86_mode(void) {
#if TARGET_MSDOS == 16
    unsigned int tmp = 0;

    if (cpu_basic_level < 3)
        return 0;

    __asm {
        .286
        smsw    tmp
    }

    return tmp & 1;
#else
    return 0;
#endif
}

void handle_packet(void) {
    unsigned int port,data;

    switch (cur_pkt_in.hdr.type) {
        case REMCTL_SERIAL_TYPE_PING:
            begin_output_packet(REMCTL_SERIAL_TYPE_PING);
            strcpy(cur_pkt_out.data,"PING");
            cur_pkt_out.hdr.length = 4;
            end_output_packet();
            break;
        case REMCTL_SERIAL_TYPE_HALT:
            halt_system = cur_pkt_in.data[0];
            begin_output_packet(REMCTL_SERIAL_TYPE_HALT);
            cur_pkt_out.data[0] = halt_system;
            cur_pkt_out.hdr.length = 1;
            end_output_packet();
            break;
        case REMCTL_SERIAL_TYPE_INPORT:
            begin_output_packet(REMCTL_SERIAL_TYPE_INPORT);
            memcpy(cur_pkt_out.data,cur_pkt_in.data,8/*big enough*/);
            cur_pkt_out.hdr.length = 3;

            port = cur_pkt_in.data[0] + (cur_pkt_in.data[1] << 8U);

            // data[2] is the I/O width
            if (cur_pkt_out.data[2] == 4) {
                // WARNING: no check is made whether your CPU is 386 or higher here!
                unsigned long ldata = inpd(port);
                cur_pkt_out.data[3] =  ldata & 0xFF;
                cur_pkt_out.data[4] = (ldata >> 8UL) & 0xFF;
                cur_pkt_out.data[5] = (ldata >> 16UL) & 0xFF;
                cur_pkt_out.data[6] = (ldata >> 24UL) & 0xFF;
                cur_pkt_out.hdr.length = 7;
            }
            else if (cur_pkt_out.data[2] == 2) {
                data = inpw(port);
                cur_pkt_out.data[3] = data & 0xFF;
                cur_pkt_out.data[4] = (data >> 8U) & 0xFF;
                cur_pkt_out.hdr.length = 5;
            }
            else {
                cur_pkt_out.data[3] = inp(port);
                cur_pkt_out.hdr.length = 4;
            }

            end_output_packet();
            break;
        case REMCTL_SERIAL_TYPE_OUTPORT:
            begin_output_packet(REMCTL_SERIAL_TYPE_OUTPORT);
            memcpy(cur_pkt_out.data,cur_pkt_in.data,8/*big enough*/);
            cur_pkt_out.hdr.length = 3;

            port = cur_pkt_in.data[0] + (cur_pkt_in.data[1] << 8U);

            // data[2] is the I/O width
            if (cur_pkt_out.data[2] == 4) {
                outpd(port,
                    (unsigned long)cur_pkt_in.data[3] +
                    ((unsigned long)cur_pkt_in.data[4] << 8UL) +
                    ((unsigned long)cur_pkt_in.data[5] << 16UL) +
                    ((unsigned long)cur_pkt_in.data[6] << 24UL));
            }
            else if (cur_pkt_out.data[2] == 2) {
                outpw(port,cur_pkt_in.data[3] + (cur_pkt_in.data[4] << 8U));
            }
            else {
                outp(port,cur_pkt_in.data[3]);
            }

            end_output_packet();
            break;
        case REMCTL_SERIAL_TYPE_MEMREAD:
            begin_output_packet(REMCTL_SERIAL_TYPE_MEMREAD);
            memcpy(cur_pkt_out.data,cur_pkt_in.data,8/*big enough*/);
            cur_pkt_out.hdr.length = 5;

            if (cur_pkt_in.data[4] != 0 && cur_pkt_in.data[4] <= 192) {
                unsigned long memaddr;

                cur_pkt_out.hdr.length = 5 + (unsigned int)cur_pkt_in.data[4];

                memaddr = ((unsigned long)cur_pkt_in.data[0]) +
                    ((unsigned long)cur_pkt_in.data[1] << 8UL) +
                    ((unsigned long)cur_pkt_in.data[2] << 16UL) +
                    ((unsigned long)cur_pkt_in.data[3] << 24UL);

#if TARGET_MSDOS == 16
                /* if any byte in the range extends past FFFF:FFFF (1MB+64KB) then use flat real mode */
                if ((memaddr+(unsigned long)cur_pkt_in.data[4]-1UL) > 0x10FFEFUL) {
                    if (cpu_basic_level >= 3 && !is_v86_mode()) {
                        if (flatrealmode_setup(FLATREALMODE_4GB)) {
                            for (port=0;port < (unsigned int)cur_pkt_in.data[4];port++)
                                cur_pkt_out.data[5+port] = flatrealmode_readb((uint32_t)memaddr + (uint32_t)port);
                        }
                        else {
                            memset(cur_pkt_out.data+5,'F',(unsigned int)cur_pkt_in.data[4]);
                        }
                    }
                    else {
                        memset(cur_pkt_out.data+5,'V',(unsigned int)cur_pkt_in.data[4]);
                    }
                }
                else {
                    unsigned long segv = (unsigned long)memaddr >> 4UL;
                    unsigned int ofsv = (unsigned int)(memaddr & 0xFUL);

                    if (segv > 0xFFFFUL) {
                        ofsv = memaddr - 0xFFFF0UL;
                        segv = 0xFFFFUL;
                    }

                    /* use fmemcpy using linear to segmented conversion */
                    _fmemcpy(cur_pkt_out.data+5,
                        MK_FP((unsigned int)segv,ofsv),(unsigned int)cur_pkt_in.data[4]);
                }
#endif
            }

            end_output_packet();
            break;
        case REMCTL_SERIAL_TYPE_MEMWRITE:
            begin_output_packet(REMCTL_SERIAL_TYPE_MEMWRITE);
            memcpy(cur_pkt_out.data,cur_pkt_in.data,8/*big enough*/);
            cur_pkt_out.hdr.length = 5;

            if (cur_pkt_in.data[4] != 0 && cur_pkt_in.data[4] <= 192) {
                unsigned long memaddr;

                memaddr = ((unsigned long)cur_pkt_in.data[0]) +
                    ((unsigned long)cur_pkt_in.data[1] << 8UL) +
                    ((unsigned long)cur_pkt_in.data[2] << 16UL) +
                    ((unsigned long)cur_pkt_in.data[3] << 24UL);

#if TARGET_MSDOS == 16
                /* if any byte in the range extends past FFFF:FFFF (1MB+64KB) then use flat real mode */
                if ((memaddr+(unsigned long)cur_pkt_in.data[4]-1UL) > 0x10FFEFUL) {
                    if (cpu_basic_level >= 3 && !is_v86_mode()) {
                        if (flatrealmode_setup(FLATREALMODE_4GB)) {
                            for (port=0;port < (unsigned int)cur_pkt_in.data[4];port++)
                                flatrealmode_writeb((uint32_t)memaddr + (uint32_t)port,cur_pkt_in.data[5+port]);
                        }
                    }
                }
                else {
                    unsigned long segv = (unsigned long)memaddr >> 4UL;
                    unsigned int ofsv = (unsigned int)(memaddr & 0xFUL);

                    if (segv > 0xFFFFUL) {
                        ofsv = memaddr - 0xFFFF0UL;
                        segv = 0xFFFFUL;
                    }

                    /* use fmemcpy using linear to segmented conversion */
                    _fmemcpy(MK_FP((unsigned int)segv,ofsv),
                        cur_pkt_in.data+5,(unsigned int)cur_pkt_in.data[4]);
                }
#endif
            }

            end_output_packet();
            break;
        default:
            begin_output_packet(REMCTL_SERIAL_TYPE_ERROR);
            cur_pkt_out_seq = 0xFF;
            end_output_packet();
            break;
    }
}

int inpkt_validate(void) {
    unsigned char sum = cur_pkt_in.hdr.chksum;
    unsigned int i;

    // the input processor make sure the mark was read, we check the rest.
    for (i=0;i < cur_pkt_in.hdr.length;i++)
        sum += cur_pkt_in.data[i];
    if (sum != 0)
        return 0;

    // and check sequence
    if (cur_pkt_in.hdr.sequence == 0xFF)
        cur_pkt_in_seq = cur_pkt_in.hdr.sequence;
    else if (cur_pkt_in.hdr.sequence != cur_pkt_in_seq)
        return 0;

    cur_pkt_in_seq = (cur_pkt_in.hdr.sequence + 1) & 0x7F;
    return 1;
}

int process_input_packet(void) {
    /* we can't generate a new packet until the current output packet is finished sending */
    if (cur_pkt_out.hdr.mark == REMCTL_SERIAL_MARK)
        return -1;

    /* send an error packet if a packet doesn't validate */
    if (inpkt_validate()) {
        handle_packet();
    }
    else {
        begin_output_packet(REMCTL_SERIAL_TYPE_ERROR);
        cur_pkt_out_seq = 0xFF;
        end_output_packet();
    }

    cur_pkt_in.hdr.mark = 0;
    cur_pkt_in_write = 0;
    process_output();
    return 0;
}

void process_input(void) {
    if (cur_pkt_in_write >= (sizeof(cur_pkt_in.hdr)+cur_pkt_in.hdr.length)) {
        if (process_input_packet() < 0)
            return;
    }

    do {
        if (!uart_8250_can_read(uart))
            break;

        ((unsigned char*)(&cur_pkt_in))[cur_pkt_in_write] = uart_8250_read(uart);
        if (cur_pkt_in_write == 0 && cur_pkt_in.hdr.mark != REMCTL_SERIAL_MARK)
            continue;

        if ((++cur_pkt_in_write) >= (sizeof(cur_pkt_in.hdr)+cur_pkt_in.hdr.length)) {
            if (process_input_packet() < 0)
                break;
        }
    } while(1);
}

void process_output(void) {
    if (cur_pkt_out.hdr.mark != REMCTL_SERIAL_MARK)
        return;

    do {
        if (!uart_8250_can_write(uart))
            break;

        uart_8250_write(uart,((unsigned char*)(&cur_pkt_out))[cur_pkt_out_write]);
        if ((++cur_pkt_out_write) >= (sizeof(cur_pkt_out.hdr)+cur_pkt_out.hdr.length)) {
            cur_pkt_out_write = 0;
            cur_pkt_out.hdr.mark = 0;
            break;
        }
    } while (1);
}

void do_check_io(void) {
    _cli();
    process_input();
    process_output();
    _sti();
}

/*--------------------------------------------------------------*/

static unsigned char devnode_raw[4096];

static void pnp_serial_scan() {
    /* most of the time the serial ports are BIOS controlled and on the motherboard.
     * they usually don't even show up in a PnP isolation scan. so we have to use
     * the "get device nodes" functions of the PnP BIOS. */
    {
        struct isa_pnp_device_node far *devn;
        unsigned int ret_ax,nodesize=0xFFFF;
        unsigned char numnodes=0xFF;
        struct isapnp_tag tag;
        unsigned char node;

        printf("Enumerating PnP system device nodes...\n");

        ret_ax = isa_pnp_bios_number_of_sysdev_nodes(&numnodes,&nodesize);
        if (ret_ax == 0 && numnodes != 0xFF && nodesize < sizeof(devnode_raw)) {
            /* NTS: How nodes are enumerated in the PnP BIOS: set node = 0, pass address of node
             *      to BIOS. BIOS, if it returns node information, will also overwrite node with
             *      the node number of the next node, or with 0xFF if this is the last one.
             *      On the last one, stop enumerating. */
            for (node=0;node != 0xFF;) {
                unsigned char far *rsc;
                int port = -1;
                int irq = -1;

                /* apparently, start with 0. call updates node to
                 * next node number, or 0xFF to signify end */
                ret_ax = isa_pnp_bios_get_sysdev_node(&node,devnode_raw,
                        ISA_PNP_BIOS_GET_SYSDEV_NODE_CTRL_NOW);

                if (ret_ax != 0)
                    break;

                devn = (struct isa_pnp_device_node far*)devnode_raw;
                if (!is_rs232_or_compat_pnp_device(devn))
                    continue;

                /* there are three config blocks, one after the other.
                 *  [allocated]
                 *  [possible]
                 *  [??]
                 * since we're not a configuration utility, we only care about the first one */
                rsc = devnode_raw + sizeof(*devn);
                if (isapnp_read_tag(&rsc,devnode_raw + devn->size,&tag)) {
                    do {
                        if (tag.tag == ISAPNP_TAG_END) /* end tag */
                            break;

                        switch (tag.tag) {
/*---------------------------------------------------------------------------------*/
case ISAPNP_TAG_IRQ_FORMAT: {
    struct isapnp_tag_irq_format far *x = (struct isapnp_tag_irq_format far*)tag.data;
    unsigned int i;
    for (i=0;i < 16 && irq < 0;i++) {
        if (x->irq_mask & (1U << (unsigned int)i))
            irq = i;
    }
} break;
case ISAPNP_TAG_IO_PORT: {
    struct isapnp_tag_io_port far *x = (struct isapnp_tag_io_port far*)tag.data;
    if (x->length >= 8 && port < 0) port = x->min_range;
} break;
case ISAPNP_TAG_FIXED_IO_PORT: {
    struct isapnp_tag_fixed_io_port far *x = (struct isapnp_tag_fixed_io_port far*)tag.data;
    if (x->length >= 8 && port < 0) port = x->base;
} break;
/*---------------------------------------------------------------------------------*/
                        };
                    } while (isapnp_read_tag(&rsc,devnode_raw + devn->size,&tag));
                }

                if (port < 0)
                    continue;

                if (add_pnp_8250(port,irq))
                    printf("Found PnP port @ 0x%03x IRQ %d\n",port,irq);
            }
        }
    }
}

void tsr_exit(void) {
    unsigned short resident_size = 0;

    /* auto-detect the size of this EXE by the MCB preceeding the PSP segment */
    /* do it BEFORE hooking in case shit goes wrong, then we can safely abort. */
    /* the purpose of this code is to compensate for Watcom C's lack of useful */
    /* info at runtime or compile time as to how large we are in memory. */
    {
        unsigned short env_seg=0;
        unsigned short psp_seg=0;
        unsigned char far *mcb;

        __asm {
            push    ax
            push    bx
            mov     ah,0x51     ; get PSP segment
            int     21h
            mov     psp_seg,bx
            pop     bx
            pop     ax
        }

        mcb = MK_FP(psp_seg-1,0);

        /* sanity check */
        if (!(*mcb == 'M' || *mcb == 'Z')/*looks like MCB*/ ||
            *((unsigned short far*)(mcb+1)) != psp_seg/*it's MY memory block*/) {
            printf("Can't figure out my resident size, aborting\n");
            abort();
        }

        resident_size = *((unsigned short far*)(mcb+3)); /* number of paragraphs */
        if (resident_size < 17) {
            printf("Resident size is too small, aborting\n");
            abort();
        }

        /* while we're at it, free our environment block as well, we don't need it */
        env_seg = *((unsigned short far*)MK_FP(psp_seg,0x2C));
        if (env_seg != 0) {
            if (_dos_freemem(env_seg) == 0) {
                *((unsigned short far*)MK_FP(psp_seg,0x2C)) = 0;
            }
            else {
                printf("WARNING: Unable to free environment block\n");
            }
        }
    }

    printf("Exiting to TSR\n");
    _dos_keep(0,resident_size);
}

static unsigned int do_exit = 0;

void mainloop(void) {
    printf("Ready. Type shift+S to exit to DOS to run in the background.\n");

    while (!do_exit) {
        if (kbhit()) {
            int c = getch();

            if (c == 27) {
                do_exit = 1;
            }
            else if (c == ' ') {
                // in case interrupts get wedged, or we're using the UART in a non-IRQ mode, check manually.
                // user can trigger this with spacebar.
                do_check_io();
            }
            else if (c == 'S') { // shift+S
                if (!use_interrupts || uart->irq == -1) {
                    printf("* Background (TSR) mode requires use of UART interrupts\n");
                }
                else {
                    tsr_exit();
                }
            }
        }

        // we have to poll here if not using interrupts
        if (!use_interrupts || uart->irq == -1)
            do_check_io();
    }
}

void help(void) {
    fprintf(stderr,"REMSRV [options]\n");
    fprintf(stderr,"Server-side end of remote control for DOS\n");
    fprintf(stderr,"  -noint                 Don't use UART interrupts\n");
    fprintf(stderr,"  -int                   Use UART interrupts (default)\n");
    fprintf(stderr,"  -baud <n>              Set BAUD rate\n");
    fprintf(stderr,"  -s <n>                 Stop bits (1 or 2)\n");
}

int parse_argv(int argc,char **argv) {
    char *a;
    int i=1;

    while (i < argc) {
        a = argv[i++];

        if (*a == '-') {
            do { a++; } while (*a == '-');

            if (!strcmp(a,"h") || !strcmp(a,"help")) {
                help();
                return 1;
            }
            else if (!strcmp(a,"noint")) {
                use_interrupts = 0;
            }
            else if (!strcmp(a,"int")) {
                use_interrupts = 1;
            }
            else if (!strcmp(a,"s")) {
                a = argv[i++];
                if (a == NULL) return 1;
                stop_bits = strtoul(a,NULL,0);
                if (stop_bits < 1) stop_bits = 1;
                else if (stop_bits > 2) stop_bits = 2;
            }
            else if (!strcmp(a,"baud")) {
                a = argv[i++];
                if (a == NULL) return 1;
                baud_rate = strtoul(a,NULL,0);
            }
            else {
                fprintf(stderr,"Unknown switch %s\n",a);
                return 1;
            }
        }
        else {
            fprintf(stderr,"Unexpected argv\n");
            return 1;
        }
    }

    return 0;
}

int main(int argc,char **argv) {
    if (parse_argv(argc,argv))
        return 1;

	cpu_probe();
	probe_dos();
    detect_windows();

    memset(&cur_pkt_in,0,sizeof(cur_pkt_in));
    memset(&cur_pkt_out,0,sizeof(cur_pkt_out));

    if (windows_mode > WINDOWS_REAL) {
        fprintf(stderr,"Do not use this program under Windows!\n");
        return 1;
    }

    if (!probe_8254()) {
        printf("8254 not found (I need this for time-sensitive portions of the driver)\n");
        return 1;
    }

    if (!probe_8259()) {
        printf("8259 not found (I need this for portions of the test involving serial interrupts)\n");
        return 1;
    }

    if (!init_8250()) {
        printf("Cannot init 8250 library\n");
        return 1;
    }

    probe_8250_bios_ports();

    {
        unsigned int i;

        for (i=0;!base_8250_full() && i < (int)bios_8250_ports;i++) {
            const uint16_t port = get_8250_bios_port(i);
            if (port == 0) continue;
            probe_8250(port);
        }
    }

    if (init_isa_pnp_bios() && find_isa_pnp_bios())
        pnp_serial_scan();

    {
        unsigned int i;
        int choice;

        printf("Which serial port should I use?\n");
        for (i=0;i < base_8250_ports;i++) {
            struct info_8250 *inf = &info_8250_port[i];
            printf("[%u] @ %03X (type %s IRQ %d)\n",i+1,inf->port,type_8250_to_str(inf->type),inf->irq);
        }
        printf("Choice? "); fflush(stdout);
        choice = -1;
        scanf("%d",&choice);
        choice--;
        if (choice < 0 || choice >= base_8250_ports) return 0;
        uart = &info_8250_port[choice];
    }

    if (uart->irq != -1 && use_interrupts) {
        printf("Using UART interrupt mode\n");

        old_timer_irq = _dos_getvect(irq2int(0));
        _dos_setvect(irq2int(0),timer_irq);

        old_irq = _dos_getvect(irq2int(uart->irq));
        _dos_setvect(irq2int(uart->irq),uart_irq);
        if (uart->irq >= 0) {
            p8259_unmask(uart->irq);
            if (uart->irq >= 8) p8259_OCW2(8,P8259_OCW2_SPECIFIC_EOI | (uart->irq&7));
            p8259_OCW2(0,P8259_OCW2_SPECIFIC_EOI | (uart->irq&7));
        }
    }

    uart_8250_enable_interrupt(uart,0); /* disable interrupts (set IER=0) */
    uart_8250_disable_FIFO(uart);
    uart_8250_set_MCR(uart,3);      /* turn on RTS and DTS */
    uart_8250_set_line_control(uart,UART_8250_LCR_8BIT | UART_8250_LCR_PARITY | (stop_bits == 2 ? UART_8250_LCR_TWO_STOP_BITS : 0)); /* 8 bit 1 stop bit odd parity */
    uart_8250_set_baudrate(uart,uart_8250_baud_to_divisor(uart,baud_rate));

    // enable FIFO
    if (uart->type > TYPE_8250_IS_16550) {
        uart_8250_set_FIFO(uart,0); // disable/flush
        uart_8250_set_FIFO(uart,1); // enable
        uart_8250_set_FIFO(uart,7); // flush in/out
        uart_8250_set_FIFO(uart,1); // enable

        if (uart->type >= TYPE_8250_IS_16750) {
            printf("Enabling FIFO (64-byte)\n");
            uart_8250_set_FIFO(uart,0xA1); // enable (bit 0), 64-byte (bit 5), threshhold is half the buffer (bits 6-7)
        }
        else {
            printf("Enabling FIFO (16-byte)\n");
            uart_8250_set_FIFO(uart,0x81); // enable (bit 0), 16-byte (bit 5), threshhold is half the buffer (bits 6-7)
        }
    }

    if (uart->irq != -1 && use_interrupts) {
        // make sure PIC has acked any pending interrupts to that line
        if (uart->irq >= 8) p8259_OCW2(8,P8259_OCW2_SPECIFIC_EOI | (uart->irq&7));
        p8259_OCW2(0,P8259_OCW2_SPECIFIC_EOI | (uart->irq&7));

        // enable interrupts. must be done HERE, not after force flushing
        uart_8250_enable_interrupt(uart,0xF);

        // force flush pending interrupts
        {
            unsigned int i;

            for (i=0;i < 256 && (inp(uart->port+PORT_8250_IIR) & 1) == 0;i++) {
                inp(uart->port);
                inp(uart->port+PORT_8250_MSR);
                inp(uart->port+PORT_8250_MCR);
                inp(uart->port+PORT_8250_LSR);
                inp(uart->port+PORT_8250_IIR);
                inp(uart->port+PORT_8250_IER);
            }
        }
    }

    // main loop
    mainloop();

    // okay, shutdown
    uart_8250_enable_interrupt(uart,0); /* disable interrupts (set IER=0) */
    uart_8250_set_MCR(uart,0);      /* RTS/DTR and aux lines off */
    uart_8250_disable_FIFO(uart);
    uart_8250_set_line_control(uart,UART_8250_LCR_8BIT | UART_8250_LCR_PARITY); /* 8 bit 1 stop bit odd parity */
    uart_8250_set_baudrate(uart,uart_8250_baud_to_divisor(uart,9600));

    if (uart->irq != -1 && use_interrupts) {
        _dos_setvect(irq2int(uart->irq),old_irq);
        if (uart->irq >= 0) p8259_mask(uart->irq);

        _dos_setvect(irq2int(0),old_timer_irq);
    }

    return 0;
}

