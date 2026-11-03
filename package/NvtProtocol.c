// Pinned copy of UniXNU/ShengNovatekTouch@a1a6f2b1912b3cde2dcbe96439e5833b9ab250f4 (NvtProtocol.c), carried in this package because a format-1 board does not
// materialise upstream components into the build tree. Keep in sync with components.toml's revision.
// SPDX-License-Identifier: Apache-2.0
// See NvtProtocol.h and NOTICE.
#include "NvtProtocol.h"
#include "NvtTouchCore.h"   // NVT_NODES: the matrix size nvt_frame_matrix fills (16x50 on this NT36523 panel)
// No <string.h> here on purpose: this file is built with -mkernel and no Darwin sysroot, so the angle include
// resolves to the host glibc header, whose __THROW/__nonnull__ machinery does not parse under that target. The two
// routines used below are declared directly instead; on arm64 size_t is unsigned long.
extern void *memcpy(void *dst, const void *src, unsigned long n);
extern void *memset(void *dst, int c, unsigned long n);
extern int memcmp(const void *a, const void *b, unsigned long n);

#define EVENT_HOST_CMD 0x50
#define EVENT_RESET_COMPLETE 0x60
#define RESET_STATE_INIT 0xa0
#define RESET_STATE_MAX 0xaf
#define HOST_CMD_ENABLE_FW_CRC 0xae

static const struct nvt_map map_single = {
    .name="NT36532", .event_buf=0x125800, .boot_rdy=0x1FB50D, .tx_auto_copy_en=0x1FC925, .spi_dma_tx_info=0x1FC914,
    .ilm_des=0x1FB528, .ilm_length=0x1FB518, .dlm_des=0x1FB52C, .dlm_length=0x1FB530,
    .g_ilm_checksum=0x1FB500, .g_dlm_checksum=0x1FB504, .r_ilm_checksum=0x1FB520, .r_dlm_checksum=0x1FB524,
    .bld_ilm_dlm_crc=0x1FB533, .enb_casc={0x1FB12C,0x01}};
static const struct nvt_map map_cascade = {
    .name="NT36532-cascade", .event_buf=0x125800, .boot_rdy=0x1FB50D, .tx_auto_copy_en=0x1FC925, .spi_dma_tx_info=0x1FC914,
    .ilm_des=0x1FB528, .ilm_length=0x1FB518, .dlm_des=0x1FB52C, .dlm_length=0x1FB530,
    .g_ilm_checksum=0x1FB500, .g_dlm_checksum=0x1FB504, .r_ilm_checksum=0x1FB520, .r_dlm_checksum=0x1FB524,
    .bld_ilm_dlm_crc=0x1FB533, .enb_casc={0x1FB12C,0x01}};
static const struct nvt_map map_e_cascade = {
    .name="NT36532E-cascade", .event_buf=0x11C400, .boot_rdy=0x1FB50D, .tx_auto_copy_en=0x1FC925, .spi_dma_tx_info=0x1FC914,
    .ilm_des=0x1FB528, .ilm_length=0x1FB518, .dlm_des=0x1FB52C, .dlm_length=0x1FB530,
    .g_ilm_checksum=0x1FB500, .g_dlm_checksum=0x1FB504, .r_ilm_checksum=0x1FB520, .r_dlm_checksum=0x1FB524,
    .bld_ilm_dlm_crc=0x1FB533, .enb_casc={0x1FB12C,0x01}};
// NT36523 (N9Z) memory map, taken verbatim from the official Windows driver map220v/nt36523_win ntfwupdate.c:19
// "NT36523 memory map". These are the only addresses this die's firmware answers on: every other map above is an
// NT36532/E literal whose download registers (0x1FB5xx) do not exist here, which is why a host download against them
// wrote into nothing and the part never left its bootloader to scan.
static const struct nvt_map map_n9z = {
    .name="NT36523", .event_buf=0x2FE00, .boot_rdy=0x3F10D, .tx_auto_copy_en=0x3F7E8, .spi_dma_tx_info=0x3F310,
    .ilm_des=0x3F128, .ilm_length=0x3F118, .dlm_des=0x3F12C, .dlm_length=0x3F130,
    .g_ilm_checksum=0x3F100, .g_dlm_checksum=0x3F104, .r_ilm_checksum=0x3F120, .r_dlm_checksum=0x3F124,
    .bld_ilm_dlm_crc=0x3F30E, .enb_casc={0x3F30E,0x01}};

// novatek_thp.c: this board's DTS says support_vendor_ic_type = <5>, which is N9Z_SERIES, and the vendor keeps a
// per-series register profile: {swrst, ver_trim, cmd, cmd_stamp}. The NB1 profile the mainline port hardcodes
// (0x1FB43E / 0x1FB104) addresses registers this part does not have, so every reset and every id read went nowhere.
#define NVT_N9Z_SWRST      0x3F0FE
#define NVT_N9Z_VER_TRIM   0x3F004
// novatek_thp.c trim_id_table: identification is a masked table match, not the three fixed bytes (0x32 0x65 0x03)
// that the mainline port looks for - that pattern is only the last entry of this table.
struct nvt_trim_entry { uint8_t id[6]; uint8_t mask[6]; };
static const struct nvt_trim_entry nvt_trim_table[] = {
    {{0x0C, 0xFF, 0xFF, 0x72, 0x66, 0x03}, {1, 0, 0, 1, 1, 1}},
    {{0x0C, 0xFF, 0xFF, 0x82, 0x66, 0x03}, {1, 0, 0, 1, 1, 1}},
    {{0x0A, 0xFF, 0xFF, 0x72, 0x65, 0x03}, {1, 0, 0, 1, 1, 1}},
    {{0x0B, 0xFF, 0xFF, 0x25, 0x65, 0x03}, {1, 0, 0, 1, 1, 1}},
    {{0x0A, 0xFF, 0xFF, 0x72, 0x67, 0x03}, {1, 0, 0, 1, 1, 1}},
    {{0x0A, 0xFF, 0xFF, 0x82, 0x66, 0x03}, {1, 0, 0, 1, 1, 1}},
    {{0x0B, 0xFF, 0xFF, 0x82, 0x66, 0x03}, {1, 0, 0, 1, 1, 1}},
    {{0x0A, 0xFF, 0xFF, 0x72, 0x66, 0x03}, {1, 0, 0, 1, 1, 1}},
    {{0x55, 0x00, 0xFF, 0x00, 0x00, 0x00}, {1, 1, 0, 1, 1, 1}},
    {{0x55, 0x72, 0xFF, 0x00, 0x00, 0x00}, {1, 1, 0, 1, 1, 1}},
    {{0xAA, 0x00, 0xFF, 0x00, 0x00, 0x00}, {1, 1, 0, 1, 1, 1}},
    {{0xAA, 0x72, 0xFF, 0x00, 0x00, 0x00}, {1, 1, 0, 1, 1, 1}},
    {{0xFF, 0xFF, 0xFF, 0x72, 0x67, 0x03}, {0, 0, 0, 1, 1, 1}},
    {{0xFF, 0xFF, 0xFF, 0x70, 0x66, 0x03}, {0, 0, 0, 1, 1, 1}},
    {{0xFF, 0xFF, 0xFF, 0x70, 0x67, 0x03}, {0, 0, 0, 1, 1, 1}},
    {{0xFF, 0xFF, 0xFF, 0x72, 0x66, 0x03}, {0, 0, 0, 1, 1, 1}},
    {{0xFF, 0xFF, 0xFF, 0x25, 0x65, 0x03}, {0, 0, 0, 1, 1, 1}},
    {{0xFF, 0xFF, 0xFF, 0x70, 0x68, 0x03}, {0, 0, 0, 1, 1, 1}},
    {{0xFF, 0xFF, 0xFF, 0x26, 0x65, 0x03}, {0, 0, 0, 1, 1, 1}},
    {{0x0B, 0xFF, 0xFF, 0x23, 0x65, 0x03}, {1, 0, 0, 1, 1, 1}},
    {{0x0A, 0xFF, 0xFF, 0x23, 0x65, 0x03}, {1, 0, 0, 1, 1, 1}},
    {{0x20, 0xFF, 0xFF, 0x23, 0x65, 0x03}, {1, 0, 0, 1, 1, 1}},
    {{0xFF, 0xFF, 0xFF, 0x32, 0x65, 0x03}, {0, 0, 0, 1, 1, 1}},
};
static int nvt_trim_match(const uint8_t *id) {
    for (unsigned l = 0; l < sizeof nvt_trim_table / sizeof nvt_trim_table[0]; ++l) {
        unsigned i = 0;
        for (; i < 6; ++i)
            if (nvt_trim_table[l].mask[i] && id[i] != nvt_trim_table[l].id[i]) break;
        if (i == 6) return 1;
    }
    return 0;
}
static int bus_write(struct nvt_chip *c, size_t len) {   // scratch[0..len) already holds [offset, data...]
    if (len > NVT_TRANSFER_LEN+1) return NVT_ERR_PARAM;
    c->scratch[0] |= 0x80;
    // The receive buffer is never empty here: this engine and this panel only ever moved bytes for a full-duplex command.
    // A transmit-only page select did not land at all, and the id read produced real data only once the same write was
    // repeated with a receive buffer (see TouchProbe.cpp busXfer), so every write carries one of the same length.
    uint8_t sink[NVT_BUS_WRITE_MAX+1];
    return c->bus->xfer(c->bus->ctx, c->scratch, sink, len) ? NVT_ERR_BUS : 0;
}
static void delay(struct nvt_chip *c, unsigned ms) {if (c->bus->delay_ms) c->bus->delay_ms(c->bus->ctx, ms);}

int nvt_set_page(struct nvt_chip *c, uint32_t addr) {
    c->scratch[0]=0xff;c->scratch[1]=(uint8_t)((addr>>15)&0xff);c->scratch[2]=(uint8_t)((addr>>7)&0xff);
    return bus_write(c,3);
}
// A write goes out as NVT_BUS_WRITE_MAX-byte messages, each addressed explicitly. This board's engine has only ever moved
// bytes for a short full-duplex burst (256 completed, 1026 failed nine times, and a transmit-only page select did not land
// at all), so the 63 KiB firmware download - the one transfer that has to arrive for the part to start scanning - is sent
// as short addressed bursts instead of one long write that never reaches SRAM. Re-addressing is legal: every burst carries
// its own page register and offset, which is exactly how the vendor library touches SRAM for its small register writes.
int nvt_write(struct nvt_chip *c, uint32_t addr, const uint8_t *data, size_t len) {
    if (!len || len > NVT_TRANSFER_LEN) return NVT_ERR_PARAM;
    for (size_t off = 0; off < len; off += NVT_BUS_WRITE_MAX) {
        size_t n = len - off < NVT_BUS_WRITE_MAX ? len - off : NVT_BUS_WRITE_MAX;
        uint32_t a = addr + (uint32_t)off;
        int r = nvt_set_page(c, a);
        if (r) return r;
        c->scratch[0] = (uint8_t)(a & 0x7f);
        memcpy(c->scratch + 1, data + off, n);
        r = bus_write(c, n + 1);
        if (r) return r;
    }
    return 0;
}
int nvt_write_byte(struct nvt_chip *c, uint32_t addr, uint8_t value) {return nvt_write(c,addr,&value,1);}
// Reads: tx = [offset, dummy, 0...], rx data starts at byte 2 (one address phase + one dummy byte).
static int read_at(struct nvt_chip *c, uint32_t offset7, uint8_t *out, size_t len) {
    if (!len || len > NVT_READ_MAX) return NVT_ERR_PARAM;
    memset(c->scratch,0,len+2);c->scratch[0]=(uint8_t)(offset7&0x7f);
    if (c->bus->xfer(c->bus->ctx,c->scratch,c->rxbuf,len+2)) return NVT_ERR_BUS;
    memcpy(out,c->rxbuf+2,len);return 0;
}
int nvt_read(struct nvt_chip *c, uint32_t addr, uint8_t *out, size_t len) {
    int r=nvt_set_page(c,addr);if (r) return r;
    return read_at(c,addr&0x7f,out,len);
}

uint32_t nvt_crc32_words(const uint8_t *data, size_t len, size_t avail) {
    uint32_t checksum=0xffffffffu;size_t words=(len+4)/4;
    for (size_t i=0;i<words;++i) {
        uint32_t w=0;
        for (unsigned b=0;b<4;++b) {size_t at=i*4+b;if (at<avail) w|=(uint32_t)data[at]<<(8*b);}
        checksum^=w;
        for (unsigned j=0;j<32;++j) checksum=(checksum&0x80000000u)?(checksum<<1)^0x04c11db7u:(checksum<<1);
    }
    return checksum;
}
static int word_at(const uint8_t *d,size_t size,size_t pos,uint32_t *out) {
    if (pos>size || size-pos<4) return NVT_ERR_FW;
    *out=(uint32_t)d[pos]|((uint32_t)d[pos+1]<<8)|((uint32_t)d[pos+2]<<16)|((uint32_t)d[pos+3]<<24);return 0;
}

int nvt_fw_parse(struct nvt_fw *fw, const uint8_t *d, size_t size) {
    memset(fw,0,sizeof(*fw));fw->data=d;fw->size=size;
    uint32_t end;
    if (size<0x40 || word_at(d,size,0,&end) || end<0x30 || end>size) return NVT_ERR_FW;
    fw->cascade_2nd_header=(d[0x20]&0x02)>>1;
    unsigned info_sec_num=0;uint32_t pos=0x30;
    if (fw->cascade_2nd_header) {
        while (pos<end/2) {++info_sec_num;pos+=0x10;}
        info_sec_num+=1;
    } else {
        while (pos<end) {++info_sec_num;pos+=0x10;}
    }
    unsigned ovly_info=(d[0x28]&0x10)>>4;
    unsigned ovly_sec_num=ovly_info?(d[0x28]&0x0f):0;
    const unsigned ilm_dlm_num=2;
    unsigned partition=ilm_dlm_num+ovly_sec_num+info_sec_num;
    if (partition>NVT_MAX_PARTITIONS) return NVT_ERR_FW;
    fw->count=partition;
    int find_bin_header=0;
    for (unsigned list=0;list<partition;++list) {
        struct nvt_fw_partition *p=&fw->part[list];
        if (list<ilm_dlm_num) {
            if (word_at(d,size,0+list*12,&p->bin_addr)||word_at(d,size,4+list*12,&p->sram_addr)||word_at(d,size,8+list*12,&p->size)) return NVT_ERR_FW;
            if ((uint64_t)p->bin_addr+p->size>size) return NVT_ERR_FW;
            // The golden CRC the bootloader compares the downloaded ILM/DLM against is stored in the header at
            // 0x18 (ILM) / 0x1C (DLM), exactly as the official NVTLoadFirmwareFile reads it (bin_map[list].crc =
            // buffer[0x18 + list*4]). Recomputing it here with nvt_crc32_words produced a value the header never
            // carries (its generator is not this CRC), which would make the part reject the image as corrupt.
            if (word_at(d,size,0x18+list*4,&p->crc)) return NVT_ERR_FW;
            memcpy(p->name,list==0?"ILM":"DLM",4);
        } else if (list<ilm_dlm_num+info_sec_num) {
            if (!find_bin_header) pos=0x30+0x10*(list-ilm_dlm_num);
            else if (fw->cascade_2nd_header) pos=end-0x10;
            if (word_at(d,size,pos,&p->sram_addr)||word_at(d,size,pos+4,&p->size)||word_at(d,size,pos+8,&p->bin_addr)||word_at(d,size,pos+12,&p->crc)) return NVT_ERR_FW;
            if (p->bin_addr<end && p->size!=0) {memcpy(p->name,"Header",7);find_bin_header=1;}
            else {p->name[0]='I';p->name[1]='n';p->name[2]='f';p->name[3]='o';p->name[4]='-';p->name[5]=(char)('0'+((list-ilm_dlm_num)%10));p->name[6]=0;}
        } else {
            pos=fw->part[1].bin_addr+0x10*(list-ilm_dlm_num-info_sec_num);
            if (word_at(d,size,pos,&p->sram_addr)||word_at(d,size,pos+4,&p->size)||word_at(d,size,pos+8,&p->bin_addr)||word_at(d,size,pos+12,&p->crc)) return NVT_ERR_FW;
            p->name[0]='O';p->name[1]='v';p->name[2]='l';p->name[3]='y';p->name[4]=0;
        }
        if ((uint64_t)p->bin_addr+p->size>size) return NVT_ERR_FW;
    }
    return 0;
}

static int write_sram(struct nvt_chip *c, const uint8_t *fw, uint32_t sram, uint32_t size, uint32_t bin) {
    while (size) {
        uint32_t len=size<NVT_TRANSFER_LEN?size:NVT_TRANSFER_LEN;
        int r=nvt_write(c,sram,fw+bin,len);if (r) return r;
        sram+=NVT_TRANSFER_LEN;bin+=NVT_TRANSFER_LEN;
        if (size<=NVT_TRANSFER_LEN) break;
        size-=NVT_TRANSFER_LEN;
    }
    return 0;
}
static int write_firmware(struct nvt_chip *c, const struct nvt_fw *fw) {
    for (unsigned i=0;i<fw->count;++i) {
        const struct nvt_fw_partition *p=&fw->part[i];
        if (!p->size) continue;
        // The vendor flow writes size+1 bytes per partition; the extra byte never leaves the firmware image.
        uint32_t size=p->size+1;
        if ((uint64_t)p->bin_addr+size>fw->size) size=p->size;
        int r=write_sram(c,fw->data,p->sram_addr,size,p->bin_addr);if (r) return r;
    }
    return 0;
}
static int write_3(struct nvt_chip *c, uint32_t addr, uint32_t v) {
    uint8_t b[3]={(uint8_t)v,(uint8_t)(v>>8),(uint8_t)(v>>16)};return nvt_write(c,addr,b,3);
}
static int write_4(struct nvt_chip *c, uint32_t addr, uint32_t v) {
    uint8_t b[4]={(uint8_t)v,(uint8_t)(v>>8),(uint8_t)(v>>16),(uint8_t)(v>>24)};return nvt_write(c,addr,b,4);
}
// novatek_thp.c touch_driver_sw_reset_idle: page select on the software-reset register, then 0xAA, then 10 ms.
static int h_sw_reset_idle(struct nvt_chip *c) {
    int r=nvt_set_page(c,c->swrst_sif);if (r) return r;
    c->scratch[0]=(uint8_t)(c->swrst_sif&0x7f);c->scratch[1]=0xAA;
    r=bus_write(c,2);delay(c,10);return r;
}

// The official NT36523 bootloader reset, disassembled from libafehalW2599Z1300.so (nvt_update_firmware @0x29140):
// a single 1-byte write of 0x69 to the software-reset register (0x3F0FE), then a 5 ms sleep
// (timespec 0x004C4B40 ns @ so+0x2a3b0). It performs NO other write - the SPI_RD_FAST (0x3F310) write the
// mainline port does is absent from both the AFE library and the kernel 3_0 driver.
static int bootloader_reset(struct nvt_chip *c) {
    int r=nvt_write_byte(c,c->swrst_sif,0x69);if (r) return r;
    delay(c,5);return 0;
}
// The vendor's very first step on power-on and on resume (novatek_thp.c touch_driver_timing_work): reset held low, the
// software-reset register written while it is held, then reset released. Exposed because the probe performs it around its
// own reset pin, before nvt_identify() runs the 0x69 / 0xAA pair again.
int nvt_soft_reset_idle(struct nvt_chip *c) {return h_sw_reset_idle(c);}
// Restart the application image without a download: page-select on the software-reset register, then the reset command.
// nt36xxx.c nvt_sw_reset_idle() sends 0xA5 and this file has always sent 0xAA; neither tree documents which one this part
// takes, so the byte is the caller's choice and the probe tries both. This is the only way back out of the bootloader
// that identify's 0x69 leaves the part in when the reset pin does not reach it.
int nvt_fw_restart(struct nvt_chip *c, uint8_t cmd) {
    int r=nvt_set_page(c,c->swrst_sif);if (r) return r;
    c->scratch[0]=(uint8_t)(c->swrst_sif&0x7f);c->scratch[1]=cmd;
    r=bus_write(c,2);delay(c,20);return r;
}
// Disassembled from libafehalW2599Z1300.so (nvt_update_firmware @0x29174-0x2921c): the bank write for each of ILM/DLM
// is exactly three SPI writes - DES gets the SRAM address (3 bytes), LENGTH gets the size (3 bytes, and the third
// byte is only bit16 of the size, ubfx w,16,#1 - the high bits are masked off), CHECKSUM gets the golden CRC
// (4 bytes). Register order: DES, LENGTH, CHECKSUM, ILM first then DLM.
static int set_bld_crc(struct nvt_chip *c, const struct nvt_fw *fw) {
    const struct nvt_map *m=c->map;int r;
    uint8_t b3[3];
    b3[0]=(uint8_t)fw->part[0].size;b3[1]=(uint8_t)(fw->part[0].size>>8);b3[2]=(uint8_t)((fw->part[0].size>>16)&1);
    if ((r=write_3(c,m->ilm_des,fw->part[0].sram_addr))||(r=nvt_write(c,m->ilm_length,b3,3))||(r=write_4(c,m->g_ilm_checksum,fw->part[0].crc))) return r;
    b3[0]=(uint8_t)fw->part[1].size;b3[1]=(uint8_t)(fw->part[1].size>>8);b3[2]=(uint8_t)((fw->part[1].size>>16)&1);
    if ((r=write_3(c,m->dlm_des,fw->part[1].sram_addr))||(r=nvt_write(c,m->dlm_length,b3,3))||(r=write_4(c,m->g_dlm_checksum,fw->part[1].crc))) return r;
    return 0;
}
// libafehalW2599Z1300.so nvt_tx_auto_copy_mode @0x27e50: one 1-byte write of 0x69 to TX_AUTO_COPY_EN (0x3F7E8).
// The stock library then calls nvt_check_spi_dma_tx_info (@0x27f50) AFTER the SRAM write: it polls a 1-byte read of
// SPI_DMA_TX_INFO (0x3F7F1) until it returns 0x00, up to 200 tries with a 1 ms sleep (timespec @ so+0x2a350) between
// them. That flag belongs to the library's DMA transport, which this driver does not use (plain SPI writes land in
// SRAM directly, the same way the kernel 3_0 driver does), so a timeout here is recorded but not fatal - the SRAM
// verify after the write is the real judge of whether the data landed.
static int tx_auto_copy_en(struct nvt_chip *c) {
    return nvt_write_byte(c,c->map->tx_auto_copy_en,0x69);
}
static int check_spi_dma_tx_info(struct nvt_chip *c) {
    for (unsigned i=0;i<200;++i) {
        uint8_t b=0xff;int r=nvt_read(c,c->map->tx_auto_copy_en+9,&b,1);if (r) return r;
        c->dma_polls=i+1;
        if (!b) {c->dma_idle=1;return 0;}
        delay(c,1);
    }
    c->dma_idle=0;return NVT_ERR_TIMEOUT;
}
static int fw_crc_enable(struct nvt_chip *c) {
    // libafehalW2599Z1300.so @0x29504-0x29530: write 0x00 (1 byte) to 0x2FE60, then 0xAE (1 byte) to 0x2FE50.
    int r;
    if ((r=nvt_write_byte(c,c->map->event_buf+EVENT_RESET_COMPLETE,0x00))) return r;
    return nvt_write_byte(c,c->map->event_buf+EVENT_HOST_CMD,HOST_CMD_ENABLE_FW_CRC);
}
// libafehalW2599Z1300.so @0x294b0-0x29500 (inlined bld_crc_enable): a 1-byte read of BLD_CRC_EN_ADDR (0x3F30E)
// followed by a 1-byte write of (value | 0x80) back to the same address. The 20-byte read the mainline port does
// (taking byte +0x10) is not what the stock library does.
static int bld_crc_enable(struct nvt_chip *c) {
    uint8_t b=0;int r;
    if ((r=nvt_read(c,c->map->bld_ilm_dlm_crc,&b,1))) return r;
    return nvt_write_byte(c,c->map->bld_ilm_dlm_crc,(uint8_t)(b|0x80));
}
static int boot_ready(struct nvt_chip *c) {
    // libafehalW2599Z1300.so @0x29534-0x2960c: read 1 byte @0x3F10D, write back (value|1), then read it again.
    uint8_t b=0;int r;
    if ((r=nvt_read(c,c->map->boot_rdy,&b,1))) return r;
    if ((r=nvt_write_byte(c,c->map->boot_rdy,(uint8_t)(b|1)))) return r;
    return nvt_read(c,c->map->boot_rdy,&b,1);
}
static int check_reset_state(struct nvt_chip *c) {
    unsigned max=c->reset_retries?c->reset_retries:10;
    for (unsigned retry=0;retry<=max;++retry) {
        uint8_t b[6]={0};int r=nvt_read(c,c->map->event_buf+EVENT_RESET_COMPLETE,b,6);if (r) return r;
        memcpy(c->dl_reset_state,b,6);c->dl_polls=retry+1;
        if (b[0]>=RESET_STATE_INIT && b[0]<=RESET_STATE_MAX) return 0;
        delay(c,10);
    }
    return NVT_ERR_TIMEOUT;
}

int nvt_identify(struct nvt_chip *c) {
    c->chip_ver_trim=NVT_N9Z_VER_TRIM;c->swrst_sif=NVT_N9Z_SWRST;c->hw_crc_len=3;
    // Exactly the sequence this board's own driver uses (novatek_thp.c: touch_driver_check_chip_ver_trim): bootloader
    // reset (0x69 @ swrst), software reset to idle (0xAA @ swrst), then read the trim register. The mainline port also
    // writes 0x5A to 0x7FFF80 first; the vendor driver never does, and that write leaves this part unresponsive.
    int r=0;
    uint8_t id[6]={0};
    int matched=0;
    for (unsigned retry=0;retry<5 && !matched;++retry) {
        if ((r=bootloader_reset(c))) return r;
        if ((r=h_sw_reset_idle(c))) return r;
        // The vendor writes the page and then reads straight away; the extra 7-byte write the mainline port does
        // here lands on the trim register itself and leaves it reading back as zeros.
        if ((r=nvt_set_page(c,c->chip_ver_trim))) return r;
        if ((r=read_at(c,c->chip_ver_trim&0x7f,id,6))) return r;
        memcpy(c->id,id,6);
        matched=nvt_trim_match(id);
        if (!matched) delay(c,10);
    }
    int matches_e=(id[0]==0x0e && id[3]==0x32 && id[4]==0x65 && id[5]==0x03);
    // NT36523 (N9Z) is id[0]==0x20 (trim table's last-but-one entry {{0x20,0xff,0xff,0x23,0x65,0x03}}), the only
    // die on this board. Its download/event registers live at 0x3Fxxx/0x2FE00, not the 0x1FBxxx the NT36532 maps use.
    int matches_n9z=(id[0]==0x20 && id[3]==0x23 && id[4]==0x65 && id[5]==0x03);
    if (!matched) return NVT_ERR_CHIP;
    // The cascade bit lives at an NB1 address; the N9Z profile in the vendor driver publishes no equivalent, so
    // treat the part as single-chip.
    c->cascade=0;
    if (matches_n9z) c->map=&map_n9z;
    else if (matches_e) c->map=c->cascade?&map_e_cascade:&map_single;
    else c->map=c->cascade?&map_cascade:&map_single;
    return 0;
}

int nvt_read_bld_crc(struct nvt_chip *c, const struct nvt_fw *fw) {
    if (!c->map || fw->count<2) return NVT_ERR_PARAM;
    uint8_t b[4]={0};int r;
    if ((r=nvt_read(c,c->map->bld_ilm_dlm_crc,b,1))) return r;
    c->crc_flags=b[0];
    const uint32_t g[2]={c->map->g_ilm_checksum,c->map->g_dlm_checksum},res[2]={c->map->r_ilm_checksum,c->map->r_dlm_checksum};
    for (unsigned i=0;i<2;++i) {
        if ((r=nvt_read(c,g[i],b,4))) return r;
        c->crc_golden[i]=(uint32_t)b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16)|((uint32_t)b[3]<<24);
        if ((r=nvt_read(c,res[i],b,4))) return r;
        c->crc_result[i]=(uint32_t)b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16)|((uint32_t)b[3]<<24);
        c->crc_bin[i]=fw->part[i].crc;
    }
    c->crc_valid=1;
    return 0;
}

static uint32_t crc_word(uint32_t checksum, uint32_t w) {
    checksum^=w;
    for (unsigned j=0;j<32;++j) checksum=(checksum&0x80000000u)?(checksum<<1)^0x04c11db7u:(checksum<<1);
    return checksum;
}
// Read ILM and DLM back completely: count differing bytes against the image and recompute the CRC over what the chip holds.
static int verify_sram_full(struct nvt_chip *c, const struct nvt_fw *fw) {
    uint8_t b[4096];
    for (unsigned p=0;p<2;++p) {
        const struct nvt_fw_partition *part=&fw->part[p];
        size_t total=((part->size+4)/4)*4;                   // the CRC covers (size+4)/4 words
        uint32_t crc=0xffffffffu,word=0;unsigned nb=0;
        c->sram_first_bad[p]=-1;c->sram_bad[p]=0;
        for (size_t off=0;off<total;off+=sizeof b) {
            size_t n=total-off<sizeof b?total-off:sizeof b;
            int r=nvt_read(c,part->sram_addr+(uint32_t)off,b,n);if (r) return r;
            for (size_t i=0;i<n;++i) {
                size_t at=off+i;
                if (at<part->size && b[i]!=fw->data[part->bin_addr+at]) {
                    if (c->sram_first_bad[p]<0) c->sram_first_bad[p]=(int32_t)at;
                    c->sram_bad[p]++;
                }
                word|=(uint32_t)b[i]<<(8*nb);
                if (++nb==4) {crc=crc_word(crc,word);word=0;nb=0;}
            }
        }
        c->sram_crc[p]=crc;
    }
    return 0;
}

int nvt_verify_sram(struct nvt_chip *c, const struct nvt_fw *fw) {
    if (!c->map || fw->count<2) return NVT_ERR_PARAM;
    (void)verify_sram_full(c,fw);
    struct {uint32_t sram,bin;} w[3]={
        {fw->part[0].sram_addr,fw->part[0].bin_addr},
        {fw->part[0].sram_addr+fw->part[0].size-32,fw->part[0].bin_addr+fw->part[0].size-32},
        {fw->part[1].sram_addr,fw->part[1].bin_addr}};
    uint8_t b[32];
    for (unsigned i=0;i<3;++i) {
        int r=nvt_read(c,w[i].sram,b,sizeof b);if (r) return r;
        c->sram_ok[i]=memcmp(b,fw->data+w[i].bin,sizeof b)==0;
    }
    c->sram_valid=1;
    return 0;
}

int nvt_download_firmware(struct nvt_chip *c, const struct nvt_fw *fw) {
    if (!c->map || fw->count<2 || !fw->part[0].size || !fw->part[1].size) return NVT_ERR_PARAM;
    int ret=0;
    for (unsigned attempt=0;attempt<3;++attempt) {
        c->dl_attempt=attempt+1;
#define NVT_STAGE(n,expr) do {c->dl_stage=(n);if ((ret=(expr))) {c->dl_rc=ret;goto next;}} while (0)
        // The complete download flow disassembled from libafehalW2599Z1300.so nvt_update_firmware, in its exact
        // order: (1) bootloader reset = one 0x69 write @0x3F0FE + 5 ms; (2) the ILM/DLM bank (DES, LENGTH with
        // bit16-only 3rd byte, golden CRC); (3) TX auto copy enable 0x69 @0x3F7E8; (4) the SRAM write of every
        // partition (size+1 bytes each); (5) poll SPI_DMA_TX_INFO 0x3F7F1 for 0x00 (part of the stock DMA
        // transport - recorded, not fatal, here); (6) bld CRC enable = 1-byte RMW |0x80 @0x3F30E; (7) fw CRC
        // enable = 0x00 @0x2FE60 then 0xAE @0x2FE50; (8) boot ready = read 0x3F10D, write back |1, read again;
        // (9) wait for the firmware's reset-complete state. Stage 5 is non-fatal because the 0x3F7F1 busy flag
        // belongs to the stock library's DMA transport and this driver writes SRAM plainly; stage 3 fails only
        // on a bus error and stays fatal. All other stages are fatal.
        NVT_STAGE(1,bootloader_reset(c));
        NVT_STAGE(2,set_bld_crc(c,fw));
        c->dl_stage=3;if ((ret=tx_auto_copy_en(c))) {c->dl_rc=ret;goto next;}
        NVT_STAGE(4,write_firmware(c,fw));
        c->dl_stage=5;if ((ret=check_spi_dma_tx_info(c))) {c->dl_rc=ret;ret=0;}   // non-fatal: evidence only
        NVT_STAGE(6,bld_crc_enable(c));
        NVT_STAGE(7,fw_crc_enable(c));
        NVT_STAGE(8,boot_ready(c));
        delay(c,5);
        NVT_STAGE(9,check_reset_state(c));
        c->dl_stage=0;
        return 0;
next:   ;
#undef NVT_STAGE
    }
    (void)nvt_read_bld_crc(c,fw);      // evidence for the failure, as the vendor driver reports it
    (void)nvt_verify_sram(c,fw);
    return ret?ret:NVT_ERR_TIMEOUT;
}

int nvt_read_frame(struct nvt_chip *c, uint8_t *frame) {
    // The vendor driver's thp_frame has the zero offset byte of CTP_SPI_READ at index 0 and the chip's data from index 1
    // (nvt_thp_read_frame -> CTP_SPI_READ(thp_frame, NVT_THP_FRAME_LEN) copies rbuf+2 into thp_frame+1), and every THP
    // header offset below is defined on that buffer. Reproduce the layout: hardware showed a valid header only one byte earlier.
    frame[0]=0;
    size_t total=NVT_THP_FRAME_LEN-1,done=0;
    while (done<total) {
        size_t n=total-done;if (n>NVT_READ_MAX) n=NVT_READ_MAX;
        int r=nvt_read(c,c->map->event_buf+(uint32_t)done,frame+1+done,n);if (r) return r;
        done+=n;
    }
    return 0;
}
// 16x50 int32, little endian, right after the 54-byte header: the frame this part's own library sizes at 3200 bytes.
void nvt_frame_matrix(const uint8_t *frame, int32_t *matrix) {
    for (int i=0;i<NVT_NODES;++i) {
        const uint8_t *p=frame+NVT_THP_MATRIX_OFFSET+i*4;
        matrix[i]=(int32_t)((uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24));
    }
}
unsigned nvt_frame_type(const uint8_t *frame) {return frame[NVT_THP_HEADER_LEN+24];}
unsigned nvt_frame_counter(const uint8_t *frame) {return (unsigned)frame[NVT_THP_HEADER_LEN+2]|((unsigned)frame[NVT_THP_HEADER_LEN+3]<<8);}

// The gate thp_afe_get_frame actually applies (0x15f10-0x15f60), in the order it applies it, and nothing else.
int nvt_thp_sync(const uint8_t *p) {
    return p[6]==NVT_THP_SYNC0 && p[7]==NVT_THP_SYNC1 && p[8]==NVT_THP_SYNC2 && p[9]==NVT_THP_SYNC3;
}
// 0x15f50-0x15f8c: the two bytes are combined base+4 high, base+5 low (bfi w8,w10,#8,#8) and compared with the
// expectation the library keeps at [ctx+380] - a replay check, not a validity check.
unsigned nvt_thp_counter(const uint8_t *p) {return ((unsigned)p[4]<<8)|(unsigned)p[5];}

static int16_t nvt_le16(const uint8_t *p) {return (int16_t)((uint16_t)p[0]|((uint16_t)p[1]<<8));}
static int16_t nvt_be16(const uint8_t *p) {return (int16_t)(((uint16_t)p[0]<<8)|(uint16_t)p[1]);}
static int32_t nvt_le32(const uint8_t *p) {return (int32_t)((uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24));}
static int32_t nvt_be32(const uint8_t *p) {return (int32_t)(((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|(uint32_t)p[3]);}

// Unfold one candidate layout of the 3200-byte payload into the 16x50 electrode grid the decoder works on. For the
// 16-bit layouts an electrode is sampled at two frequencies (project feature 0x78 sets MULTI_FREQ_NOISE), so the two
// samples are averaged: they measure the same node, and averaging is what makes the noise band the CSV declares
// (-300..300) smaller than the peak the decoder needs.
void nvt_thp_nodes(const uint8_t *payload, unsigned variant, int32_t *matrix) {
    for (int e=0;e<NVT_NODES;++e) {
        const int row=e/NVT_COLUMNS, col=e%NVT_COLUMNS;
        int32_t v=0;
        switch(variant) {
        case NVT_THP_V_LE_PAIR:
            v=((int32_t)nvt_le16(payload+2*((2*row)*NVT_COLUMNS+col))+(int32_t)nvt_le16(payload+2*((2*row+1)*NVT_COLUMNS+col)))/2;
            break;
        case NVT_THP_V_BE_PAIR:
            v=((int32_t)nvt_be16(payload+2*((2*row)*NVT_COLUMNS+col))+(int32_t)nvt_be16(payload+2*((2*row+1)*NVT_COLUMNS+col)))/2;
            break;
        case NVT_THP_V_LE_BLOCK:
            v=((int32_t)nvt_le16(payload+2*e)+(int32_t)nvt_le16(payload+2*(NVT_NODES+e)))/2;
            break;
        case NVT_THP_V_BE_BLOCK:
            v=((int32_t)nvt_be16(payload+2*e)+(int32_t)nvt_be16(payload+2*(NVT_NODES+e)))/2;
            break;
        case NVT_THP_V_LE32: v=nvt_le32(payload+4*e); break;
        case NVT_THP_V_BE32: v=nvt_be32(payload+4*e); break;
        default: break;
        }
        matrix[e]=v;
    }
}

unsigned nvt_thp_roughness(const int32_t *m) {
    unsigned long sum=0;
    unsigned gaps=0;
    for(int r=0;r<NVT_ROWS;++r)
        for(int c=1;c<NVT_COLUMNS;++c) {
            long d=(long)m[r*NVT_COLUMNS+c]-m[r*NVT_COLUMNS+c-1];
            sum+=(unsigned long)(d<0?-d:d);
            ++gaps;
        }
    return gaps ? (unsigned)(sum/gaps) : 0xffffffffu;
}

int nvt_get_fw_info(struct nvt_chip *c, struct nvt_fw_info *info) {
    uint8_t b[38];
    memset(info,0,sizeof *info);
    for (int attempt=0;attempt<4;++attempt) {
        int r=nvt_read(c,c->map->event_buf+NVT_EVENT_FWINFO,b,sizeof b);if (r) return r;
        if ((uint8_t)(b[0]+b[1])!=0xff) continue;
        info->fw_ver=b[0];info->x_num=b[2];info->y_num=b[3];
        info->abs_x_max=(uint16_t)((b[4]<<8)|b[5]);info->abs_y_max=(uint16_t)((b[6]<<8)|b[7]);
        info->fw_type=b[13];info->pid=(uint16_t)((b[35]<<8)|b[34]);
        return 0;
    }
    info->defaulted=1;
    return NVT_ERR_FW;
}
int nvt_set_custom_cmd(struct nvt_chip *c, uint8_t cmd, uint16_t value) {
    delay(c,35);
    for (int tries=0;tries<5;++tries) {
        uint8_t b[5]={0xbf,cmd,0x00,(uint8_t)value,(uint8_t)(value>>8)};
        int r=nvt_write(c,c->map->event_buf+NVT_EVENT_HOST_CMD,b,sizeof b);if (r) return r;
        delay(c,20);
        uint8_t st=0xff;r=nvt_read(c,c->map->event_buf+NVT_EVENT_HOST_CMD,&st,1);if (r) return r;
        if (!st) return 0;
    }
    return NVT_ERR_TIMEOUT;
}
int nvt_set_doze_delay(struct nvt_chip *c, uint16_t value) {
    uint8_t b[2]={(uint8_t)value,(uint8_t)(value>>8)};
    return nvt_write(c,NVT_DOZE_DELAY_ADDR,b,sizeof b);
}
int nvt_start_scanning(struct nvt_chip *c, struct nvt_fw_info *info) {
    int r=nvt_get_fw_info(c,info);                 // a bad info block is reported but does not stop the sequence
    int info_err=r;
    if (r==NVT_ERR_BUS) return r;
    if ((r=nvt_set_custom_cmd(c,0x19,0x00))) return r;   // idle baseline update
    if ((r=nvt_set_doze_delay(c,120))) return r;
    if ((r=nvt_set_custom_cmd(c,0x01,0x02))) return r;   // doze mode
    return info_err;
}
int nvt_peek_valid(const uint8_t *h) {
    uint16_t crc=(uint16_t)(h[4]|(h[5]<<8)),inv=(uint16_t)(h[12]|(h[13]<<8));
    uint32_t mk=(uint32_t)h[8]|((uint32_t)h[9]<<8)|((uint32_t)h[10]<<16)|((uint32_t)h[11]<<24);
    uint32_t mi=(uint32_t)h[16]|((uint32_t)h[17]<<8)|((uint32_t)h[18]<<16)|((uint32_t)h[19]<<24);
    return inv==(uint16_t)~crc && mi==~mk;
}
int nvt_frame_header_valid(const uint8_t *frame) {return nvt_peek_valid(frame+NVT_THP_HEADER_LEN);}
unsigned nvt_peek_counter(const uint8_t *h) {return (unsigned)h[2]|((unsigned)h[3]<<8);}
unsigned nvt_peek_type(const uint8_t *h) {return h[24];}
int nvt_peek_frame_header(struct nvt_chip *c, uint8_t *hdr) {
    // frame[i] = chip byte i-1 (see nvt_read_frame), so frame[NVT_THP_HEADER_LEN] is chip byte NVT_THP_HEADER_LEN-1.
    return nvt_read(c,c->map->event_buf+NVT_THP_HEADER_LEN-1,hdr,NVT_THP_PEEK_LEN);
}
// The event-buffer base is the one board constant the pinned protocol copy gets wrong for this panel: 0x125800 is
// an SM8550 die's address and this part has no such SRAM, so every read from it returned unrelated bytes, the header
// check never matched, and the "start scanning" host command was written where no firmware listens - which is why the
// interrupt line stayed low for the whole run. These two entry points let the board pick the base by measurement.
static struct nvt_map map_runtime;
void nvt_select_event_buf(struct nvt_chip *c, uint32_t base) {
    map_runtime = *c->map;
    map_runtime.event_buf = base;
    c->map = &map_runtime;
}
int nvt_probe_event_buf(struct nvt_chip *c, uint32_t base, struct nvt_buf_probe *p) {
    p->base = base; p->first = 0; p->valid = 0; p->cmd_rc = -1;
    nvt_select_event_buf(c, base);
    uint8_t h[NVT_THP_PEEK_LEN];
    if (!nvt_peek_frame_header(c, h))
        p->first = (uint32_t)h[0]|((uint32_t)h[1]<<8)|((uint32_t)h[2]<<16)|((uint32_t)h[3]<<24),
        p->valid = nvt_peek_valid(h);
    // A firmware that owns this base consumes a host command: the status byte falls back to zero. A base that holds
    // unrelated memory leaves the 0xbf marker in place, so this separates "no frames yet" from "wrong address".
    p->cmd_rc = nvt_set_custom_cmd(c, 0x19, 0x00);
    return 0;
}
// Locate the event structure instead of assuming a base. A running firmware writes its reset state (0xA0..0xAF, the
// vendor's RST_COMPLETE_STATE) at event_buf+0x60 and its info block at event_buf+0x78, whose first two bytes always
// sum to 0xff. Both have to hold at the same offset, which is specific enough to sweep a whole window and is written
// by the firmware itself - unlike 0x125800, a literal from another die that answers with unrelated bytes here.
int nvt_scan_event_buf(struct nvt_chip *c, uint32_t lo, uint32_t hi, struct nvt_buf_scan *out) {
    if (!out || hi <= lo) return NVT_ERR_PARAM;
    static uint8_t blk[2048];
    out->base = 0; out->state = 0; out->hits = 0; out->blocks_ok = 0; out->blocks_bad = 0;
    for (uint32_t a = lo; a < hi; a += sizeof blk) {
        size_t len = (hi - a) < sizeof blk ? (size_t)(hi - a) : sizeof blk;
        if (len < 0x80) break;
        if (nvt_read(c, a, blk, len)) {++out->blocks_bad; continue;}
        ++out->blocks_ok;
        for (size_t o = 0; o + 0x80 <= len; ++o) {
            const uint8_t st = blk[o + 0x60];
            if (st < 0xA0 || st > 0xAF) continue;
            if ((uint8_t)(blk[o + 0x78] + blk[o + 0x79]) != 0xff) continue;
            if (!out->hits) {out->base = a + (uint32_t)o; out->state = st;}
            ++out->hits;
        }
    }
    return out->hits ? 0 : NVT_ERR_TIMEOUT;
}

// novatek_thp.c, touch_driver_get_frame(): "write cmd 0x20 and get frame". The command byte (spi_read_mask(0x20)==0x20,
// bit 7 clear means read) goes out in the first tx slot and the reply of that same full-duplex transfer is the frame,
// which the vendor driver passes straight to the HAL with no byte removed: touch_driver_get_frame() writes w_buf[0]=0x20
// and hands the whole rx buffer up, no RX_DATA_START shift. Nothing is paged in and no address is sent.
int nvt_n9z_read_frame(struct nvt_chip *c, uint8_t *out, size_t len) {
    // The transfer is tx==rx==len exactly as the vendor calls it. The first byte clocked out of the part while the 0x20
    // command is still on the wire is a dummy, so the report's own byte 0 lands at rx[1] - but that shift is the report
    // decoder's business (nvt_n9z_report tries every start offset), not the transport's: the transport must return the
    // raw reply verbatim, byte for byte, so the decoder sees exactly what the HAL sees.
    if (!len || len > NVT_FRAME_READ_MAX) return NVT_ERR_PARAM;
    memset(c->scratch, 0, len);
    memset(c->rxbuf, 0, len);
    c->scratch[0] = 0x20;
    if (c->bus->xfer(c->bus->ctx, c->scratch, c->rxbuf, len)) return NVT_ERR_BUS;
    memcpy(out, c->rxbuf, len);
    return 0;
}

// The report the N9Z firmware streams is a flat list of six-byte slots. Slot 0 may sit at byte 0, 1 or 2 of the burst
// depending on how many address/dummy bytes the caller kept in front of it, so every offset is tried.
//
// A list is only believed when it ends the way a report ends - on an empty slot (id 0) or at the end of the burst - and
// when every coordinate it holds is inside this panel (devicetree abs_max 1600 x 2560). Without both checks a random
// burst produces a "contact" every few frames: an id is valid in 10 of 32 byte values and an event in 2 of 8, so the
// bare format test accepts about one position in thirteen.
#define N9Z_MAX_X 1600
#define N9Z_MAX_Y 2560
static int n9z_slots(const uint8_t *f, size_t len, struct nvt_point *out, int max, int start, int *ok) {
    int n = 0;
    *ok = 0;
    for (;;) {
        const size_t p = (size_t)start + (size_t)6 * (size_t)n;
        if (p + 6 > len) {*ok = 1; break;}                 // the burst ended: a list that runs to the end is complete
        const int id = f[p] >> 3, ev = f[p] & 0x07;
        if (id == 0) {*ok = 1; break;}                     // empty slot: the list ends here
        if (id > 10 || (ev != 1 && ev != 2)) break;
        // Coordinate packing from the official NT36523 driver (map220v/nt36523_win, ntinternal.c:175-178,
        // the public NT36523 decoder for the same 1600x2560 panel): x is the low byte shifted up four bits
        // plus the high nibble of byte 3, y is the middle byte shifted up four bits plus the low nibble.
        const int x = ((int)f[p + 1] << 4) | (f[p + 3] >> 4);
        const int y = ((int)f[p + 2] << 4) | (f[p + 3] & 0x0f);
        if (x > N9Z_MAX_X || y > N9Z_MAX_Y) break;
        if (out && n < max) {
            out[n].id = id; out[n].event = ev; out[n].x = x; out[n].y = y;
            out[n].area = f[p + 4];
            out[n].pressure = f[p + 5];
        }
        ++n;
        if (n >= max) {*ok = 1; break;}
    }
    return n;
}
int nvt_n9z_report(const uint8_t *frame, size_t len, struct nvt_point *out, int max, int *start) {
    int best = 0, bestAt = 0;
    if (max < 1) return 0;
    for (int at = 0; at < 4; ++at) {
        int n = 0, ok = 0;
        // The scan has to use the same cap as the real parse: with a cap of zero every single slot would end the loop as
        // "complete", and one accidental record would then be enough to pick a start offset.
        n = n9z_slots(frame, len, 0, max, at, &ok);
        if (ok && n > best) {best = n; bestAt = at;}
    }
    if (start) *start = bestAt;
    if (!out || !best) return 0;
    int n = 0, ok = 0;
    n = n9z_slots(frame, len, out, max, bestAt, &ok);
    return n > max ? max : n;
}

// One slot of the vendor's point report, decoded exactly as novatek_irq_bottom_half() does it. Kept as a helper so the
// three candidate start offsets below share one copy of the rules.
static int n9z_point_slot(const uint8_t *f, size_t len, size_t p, int span_x, int span_y, struct nvt_point *out) {
    if (p + 6 > len) return 0;
    const int id = f[p] >> 3, ev = f[p] & 0x07;
    if (id == 0 || id > 10) return 0;                       // empty slot, or an id the part never issues
    // 00 not present, 01 enter, 02 moving, 03 up (the vendor's own break), 06 glove. 04/05/07 are reserved and the
    // vendor's if/else-if chain simply does not report them, so neither do we.
    if (ev != 0x01 && ev != 0x02 && ev != 0x06) return 0;
    const int x = ((int)f[p + 1] << 4) + (int)(f[p + 3] >> 4);
    const int y = ((int)f[p + 2] << 4) + (int)(f[p + 3] & 0x0F);
    if (x < 0 || y < 0 || x > span_x || y > span_y) return 0;
    if (out) {
        out->x = x; out->y = y; out->id = id; out->event = ev;
        out->area = f[p + 4];                               // INPUT_MAJOR_OFFSET: the finger's major axis
        out->pressure = f[p + 5] ? f[p + 5] : 1;            // never zero: the vendor forces 1 for its own consumer
    }
    return 1;
}

int nvt_n9z_points(const uint8_t *frame, size_t len, int span_x, int span_y, struct nvt_point *out, int max,
                   int *start, int *wdt) {
    if (wdt) *wdt = 0;
    if (max < 1) return 0;
    if (len >= 7) {
        int held = 1;
        for (int q = 1; q <= 6; ++q)
            if (frame[q] != 0xFD && frame[q] != 0xFE) {held = 0; break;}
        *wdt = held;
    }
    int best = 0, bestAt = 0;
    for (int at = 0; at < 3; ++at) {
        int n = 0;
        for (int i = 0; i < max; ++i)
            if (n9z_point_slot(frame, len, (size_t)at + 1 + 6 * (size_t)i, span_x, span_y, 0)) ++n;
        if (n > best) {best = n; bestAt = at;}
    }
    if (start) *start = bestAt;
    if (!out || !best) return 0;
    int n = 0;
    for (int i = 0; i < max; ++i)
        if (n9z_point_slot(frame, len, (size_t)bestAt + 1 + 6 * (size_t)i, span_x, span_y, n < max ? &out[n] : 0)) ++n;
    return n;
}
