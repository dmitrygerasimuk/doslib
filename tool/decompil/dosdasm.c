#include "minx86dec/types.h"
#include "minx86dec/state.h"
#include "minx86dec/opcodes.h"
#include "minx86dec/coreall.h"
#include "minx86dec/opcodes_str.h"
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>

#include <hw/dos/exehdr.h>

#ifndef O_BINARY
#define O_BINARY 0
#endif

uint8_t                         dec_buffer[256];
uint8_t*                        dec_read;
uint8_t*                        dec_end;
char                            arg_c[101];
struct minx86dec_state          dec_st;
struct minx86dec_instruction    dec_i;
minx86_read_ptr_t               iptr;
uint16_t                        entry_cs,entry_ip;
uint32_t                        start_decom,end_decom,entry_ofs;
uint32_t                        current_offset;

struct exe_dos_header           exehdr;

char*                           src_file = NULL;
int                             src_fd = -1;

uint32_t current_offset_minus_buffer() {
    return current_offset - (uint32_t)(dec_end - dec_buffer);
}

static void minx86dec_init_state(struct minx86dec_state *st) {
	memset(st,0,sizeof(*st));
}

static void minx86dec_set_buffer(struct minx86dec_state *st,uint8_t *buf,int sz) {
	st->fence = buf + sz;
	st->prefetch_fence = dec_buffer + sizeof(dec_buffer) - 16;
	st->read_ip = buf;
}

int parse_argv(int argc,char **argv) {
    char *a;
    int i=1;

    while (i < argc) {
        a = argv[i++];

        if (*a == '-') {
            do { a++; } while (*a == '-');

            if (!strcmp(a,"i")) {
                src_file = argv[i++];
                if (src_file == NULL) return 1;
            }
            else {
                fprintf(stderr,"Unknown switch %s\n",a);
                return 1;
            }
        }
        else {
            fprintf(stderr,"Unknown arg %s\n",a);
            return 1;
        }
    }

    if (src_file == NULL) {
        fprintf(stderr,"Must specify -i source file\n");
        return 1;
    }

    return 0;
}

int refill() {
    const size_t flush = sizeof(dec_buffer) / 2;
    const size_t padding = 16;
    size_t dlen;

    if (dec_end > dec_read)
        dlen = (size_t)(dec_end - dec_read);
    else
        dlen = 0;

    if (dec_read >= (dec_buffer+flush)) {
        assert((dec_read+dlen) <= (dec_buffer+sizeof(dec_buffer)-padding));
        if (dlen != 0) memmove(dec_buffer,dec_read,dlen);
        dec_read = dec_buffer;
        dec_end = dec_buffer + dlen;
    }
    {
        unsigned char *e = dec_buffer + sizeof(dec_buffer) - padding;

        if (dec_end < e) {
            unsigned long clen = end_decom - current_offset;
            dlen = (size_t)(e - dec_end);
            if ((unsigned long)dlen > clen)
                dlen = (size_t)clen;

            if (dlen != 0) {
                int rd = read(src_fd,dec_end,dlen);
                if (rd > 0) {
                    dec_end += rd;
                    current_offset += (unsigned long)rd;
                }
            }
        }
    }

    return (dec_read < dec_end);
}

int main(int argc,char **argv) {
    uint16_t dec_cs;
    int c;

    if (parse_argv(argc,argv))
        return 1;

    src_fd = open(src_file,O_RDONLY|O_BINARY);
    if (src_fd < 0) {
        fprintf(stderr,"Unable to open %s, %s\n",src_file,strerror(errno));
        return 1;
    }

    if (lseek(src_fd,0,SEEK_SET) == 0 && read(src_fd,&exehdr,sizeof(exehdr)) == (int)sizeof(exehdr) && exehdr.magic == 0x5A4DU/*MZ*/) {
        unsigned long img_res_size = exe_dos_header_file_resident_size(&exehdr);
        unsigned long hdr_size = exe_dos_header_file_header_size(&exehdr);
        if (img_res_size >= hdr_size) img_res_size -= hdr_size;
        else img_res_size = 0;
        // EXE
        start_decom = hdr_size;
        end_decom = hdr_size + img_res_size;
        entry_cs = 0;
        entry_ip = 0;
        entry_ofs = 0;
        dec_cs = entry_cs;

        printf(".EXE decompile, %lu (0x%lx) resident bytes\n",(unsigned long)img_res_size,(unsigned long)img_res_size);
        printf("CS:IP entry point is %04lx:%04lx (linear 0x%08lx file 0x%08lx)\n",
            (unsigned long)exehdr.init_code_segment,
            (unsigned long)exehdr.init_instruction_pointer,
            ((((unsigned long)exehdr.init_code_segment << 4UL) + (unsigned long)exehdr.init_instruction_pointer) & 0xFFFFFUL),
            ((((unsigned long)exehdr.init_code_segment << 4UL) + (unsigned long)exehdr.init_instruction_pointer) & 0xFFFFFUL) + start_decom);
    }
    else {
        // COM
        start_decom = 0;
        end_decom = lseek(src_fd,0,SEEK_END);
        entry_cs = 0x0000U;
        entry_ip = 0x0100U;
        entry_ofs = 0;
        dec_cs = 0;

        printf(".COM decompile\n");
    }

    printf("Starting at %lu (0x%08lx), ending at %lu (0x%08lx)\n",
        (unsigned long)start_decom,
        (unsigned long)start_decom,
        (unsigned long)end_decom,
        (unsigned long)end_decom);
    printf("Starting decode CS:IP %04lX:%04lX offset 0x%lx\n",
        (unsigned long)entry_cs,
        (unsigned long)entry_ip,
        (unsigned long)entry_ofs);

    current_offset = start_decom;
	minx86dec_init_state(&dec_st);
    dec_read = dec_end = dec_buffer;
	dec_st.data32 = dec_st.addr32 = 0;
    if ((uint32_t)lseek(src_fd,start_decom,SEEK_SET) != start_decom)
        return 1;

    do {
        uint32_t ip = (uint32_t)(dec_read - dec_buffer) + current_offset_minus_buffer() + entry_ip - start_decom - (dec_cs << 4UL);

        if (ip >= 0x10000UL) {
            dec_cs += 0x1000UL;
            entry_ip = 0;
            continue;
        }

        if (!refill()) break;

        minx86dec_set_buffer(&dec_st,dec_read,(int)(dec_end - dec_read));
        minx86dec_init_instruction(&dec_i);
		dec_st.ip_value = ip;
		minx86dec_decodeall(&dec_st,&dec_i);
        assert(dec_i.end >= dec_read);
        assert(dec_i.end <= (dec_buffer+sizeof(dec_buffer)));

		printf("%04lX:%04lX @0x%08lX ",(unsigned long)dec_cs,(unsigned long)dec_st.ip_value,(unsigned long)(dec_read - dec_buffer) + current_offset_minus_buffer());
		for (c=0,iptr=dec_i.start;iptr != dec_i.end;c++)
			printf("%02X ",*iptr++);
		for (;c < 8;c++)
			printf("   ");
		printf("%-8s ",opcode_string[dec_i.opcode]);
		for (c=0;c < dec_i.argc;) {
			minx86dec_regprint(&dec_i.argv[c],arg_c);
			printf("%s",arg_c);
			if (++c < dec_i.argc) printf(",");
		}
		if (dec_i.lock) printf("  ; LOCK#");
		printf("\n");

        dec_read = dec_i.end;
    } while(1);

    close(src_fd);
	return 0;
}
