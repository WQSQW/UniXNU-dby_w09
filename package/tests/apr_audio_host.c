/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host tests for package/ShengApr.c: the byte layout of every request this board's legacy audio stack sends, checked against the
 * field order and sizes transcribed from techpack/audio. Nothing here touches hardware. */
#include <stdio.h>
#include <string.h>

#include "../ShengApr.h"

static int failures;

static void check(int ok, const char *what)
{
    if (!ok) {printf("FAIL %s\n", what); ++failures;}
}

static void check32(const uint8_t *p, uint32_t offset, uint32_t want, const char *what)
{
    const uint32_t got = (uint32_t)p[offset] | (uint32_t)p[offset + 1] << 8 | (uint32_t)p[offset + 2] << 16 |
                         (uint32_t)p[offset + 3] << 24;
    if (got != want) {printf("FAIL %s: offset %u is 0x%x, want 0x%x\n", what, offset, got, want); ++failures;}
}

static void check16(const uint8_t *p, uint32_t offset, uint16_t want, const char *what)
{
    const uint16_t got = (uint16_t)((uint16_t)p[offset] | (uint16_t)p[offset + 1] << 8);
    if (got != want) {printf("FAIL %s: offset %u is 0x%x, want 0x%x\n", what, offset, got, want); ++failures;}
}

/* Small writers for the answer packets this test has to build by hand. */
static void put_le32(uint8_t *p, uint32_t offset, uint32_t v)
{
    p[offset] = (uint8_t)v; p[offset + 1] = (uint8_t)(v >> 8); p[offset + 2] = (uint8_t)(v >> 16); p[offset + 3] = (uint8_t)(v >> 24);
}
static void put_le16(uint8_t *p, uint32_t offset, uint16_t v) {p[offset] = (uint8_t)v; p[offset + 1] = (uint8_t)(v >> 8);}

static void check_header(const uint8_t *p, uint32_t size, uint8_t service, uint16_t port, uint32_t token, uint32_t opcode)
{
    check16(p, 0, APR_HDR_FIELD(AprMsgSeqCmd, APR_HEADER_BYTES, APR_PKT_VER), "hdr_field");
    check16(p, 2, (uint16_t)size, "pkt_size");
    check(p[4] == service && p[8] == service, "src_svc == dest_svc");
    check(p[5] == AprDomainAPPS && p[9] == AprDomainADSP, "src_domain APPS, dest_domain ADSP");
    check16(p, 6, port, "src_port");
    check16(p, 10, port, "dest_port");
    check32(p, 12, token, "token");
    check32(p, 16, opcode, "opcode");
}

static void test_core(void)
{
    uint8_t buf[APR_MAX_PACKET];
    const uint32_t n = apr_core_get_state(buf, sizeof buf, 7);
    check(n == 20, "core get state is header only");
    check_header(buf, n, AprSvcAdspCore, 0, 7, APR_OP_CORE_GET_STATE);

    const uint32_t v = apr_avcs_get_fwk_version(buf, sizeof buf);
    check(v == 20, "avcs fwk version is header only, ports 0, token 0");
    check_header(buf, v, AprSvcAdspCore, 0, 0, APR_OP_AVCSS_GET_FWK_VERSION);

    /* The answer the vendor caches: five u32 then one record of three u32 per service. */
    uint8_t rsp[96];
    const uint32_t o = APR_HEADER_BYTES;
    memcpy(rsp, buf, APR_HEADER_BYTES);
    put_le32(rsp, o, 1); put_le32(rsp, o + 4, 2); put_le32(rsp, o + 8, 3); put_le32(rsp, o + 12, 4);
    put_le32(rsp, o + 16, 3);
    put_le32(rsp, o + 20, APRV2_IDS_SERVICE_ID_ADSP_CORE_V); put_le32(rsp, o + 24, 5); put_le32(rsp, o + 28, 0);
    put_le32(rsp, o + 32, APRV2_IDS_SERVICE_ID_ADSP_AFE_V); put_le32(rsp, o + 36, 3); put_le32(rsp, o + 40, 0);
    put_le32(rsp, o + 44, APRV2_IDS_SERVICE_ID_ADSP_ASM_V); put_le32(rsp, o + 48, 4); put_le32(rsp, o + 52, 0);
    const uint32_t size = APR_HEADER_BYTES + 20u + 3u * 12u;
    put_le16(rsp, 0, APR_HDR_FIELD(AprMsgCmdRsp, APR_HEADER_BYTES, APR_PKT_VER));
    put_le16(rsp, 2, (uint16_t)size);
    rsp[5] = AprDomainADSP; rsp[9] = AprDomainAPPS;                 /* an answer comes from the DSP, not the other way round */
    put_le32(rsp, 16, APR_OP_AVCSS_GET_FWK_VERSION_RSP);
    AprMessage m;
    check(apr_decode(rsp, size, &m) == AprOk, "decode fwk version answer");
    check(m.payload_bytes == 56, "the answer carries five u32 and three records");
    uint32_t api = 0, count = 0;
    check(apr_avcs_service_api(&m, APRV2_IDS_SERVICE_ID_ADSP_AFE_V, &api, &count) == 0, "AFE api version found");
    check(api == AFE_API_VERSION_V3 && count == 3, "AFE api v3, three services listed");
    check(apr_avcs_service_api(&m, APRV2_IDS_SERVICE_ID_ADSP_ADM_V, &api, &count) != 0, "an absent service is an error");
    m.payload_bytes = 20u + 2u * 12u;                                        /* two of the three records: must be refused */
    check(apr_avcs_service_api(&m, APRV2_IDS_SERVICE_ID_ADSP_AFE_V, &api, &count) != 0, "truncated record list refused");
}

static void test_afe(void)
{
    uint8_t buf[APR_MAX_PACKET];

    /* The Primary TDM bit clock, the family this board sends by default (instance id support off). */
    const AprClkSet clk = {AFE_CLK_ID_PRI_TDM_IBIT, AFE_TDM_CLK_HZ, 1, AFE_CLK_ATTR_INVERT_COUPLE_NO, AFE_CLK_ROOT_DEFAULT};
    uint32_t n = apr_afe_clock_set(buf, sizeof buf, &clk);
    check(n == 68, "clock set packet is 68 bytes with the v1 header");
    check_header(buf, n, AprSvcAfe, 0, AFE_CLK_TOKEN, APR_OP_AFE_SVC_SET_PARAM);
    check32(buf, 20, 32, "svc v1 payload_size covers the v1 header and the clock struct");
    for (uint32_t i = 24; i < 36; ++i) check(buf[i] == 0, "svc v1 memory header is zero in band");
    check32(buf, 36, AFE_MODULE_CLOCK_SET, "clock module");
    check32(buf, 40, AFE_PARAM_ID_CLOCK_SET, "clock param");
    check16(buf, 44, 20, "struct afe_clk_set is 20 bytes");
    check16(buf, 46, 0, "v1 header reserved");
    check32(buf, 48, AFE_CLK_API_VERSION, "clock minor version");
    check32(buf, 52, AFE_CLK_ID_PRI_TDM_IBIT, "clk_id PRI_TDM_IBIT");
    check32(buf, 56, AFE_TDM_CLK_HZ, "6.144 MHz for four 32 bit slots at 48 kHz");
    check16(buf, 60, AFE_CLK_ATTR_INVERT_COUPLE_NO, "clk_attri");
    check16(buf, 62, AFE_CLK_ROOT_DEFAULT, "clk_root");
    check32(buf, 64, 1, "enable");

    /* The same packet in the v3 family, for the record. */
    apr_set_iid_supported(1);
    n = apr_afe_clock_set(buf, sizeof buf, &clk);
    check(n == 72, "clock set packet is 72 bytes with the v3 header");
    check_header(buf, n, AprSvcAfe, 0, AFE_CLK_TOKEN, APR_OP_AFE_SVC_SET_PARAM_V2);
    for (uint32_t i = 20; i < 32; ++i) check(buf[i] == 0, "svc v2 memory header comes first and is zero");
    check32(buf, 32, 36, "svc v2 payload_size covers the v3 header and the clock struct");
    check32(buf, 36, AFE_MODULE_CLOCK_SET, "v3 module");
    check16(buf, 40, 0, "v3 instance id");
    check16(buf, 42, 0, "v3 reserved");
    check32(buf, 44, AFE_PARAM_ID_CLOCK_SET, "v3 param id");
    check32(buf, 48, 20, "v3 param size");
    check32(buf, 52, AFE_CLK_API_VERSION, "v3 clock minor version");
    apr_set_iid_supported(0);

    /* TDM configuration of port 0x9000 as this board's device tree describes it. */
    const AprTdmCfg tdm = {4, 48000, 16, AFE_LINEAR_PCM_DATA, AFE_PORT_TDM_SHORT_SYNC_SLOT_MODE, AFE_PORT_TDM_SYNC_SRC_INTERNAL,
                           4, 0, 1, 1, 32, 0x0000000Fu};
    n = apr_afe_tdm_config(buf, sizeof buf, AFE_PORT_IDX_PRIMARY_TDM_RX, AFE_PORT_ID_PRIMARY_TDM_RX, &tdm);
    check(n == 84, "tdm config packet is 84 bytes with the v1 header");
    check_header(buf, n, AprSvcAfe, 0, AFE_PORT_IDX_PRIMARY_TDM_RX, APR_OP_AFE_PORT_SET_PARAM_V2);
    check16(buf, 20, AFE_PORT_ID_PRIMARY_TDM_RX, "port_id");
    check16(buf, 22, 48, "port v2 payload_size");
    for (uint32_t i = 24; i < 36; ++i) check(buf[i] == 0, "port v2 memory header is zero");
    check32(buf, 36, AFE_MODULE_AUDIO_DEV_INTERFACE, "tdm module");
    check32(buf, 40, AFE_PARAM_ID_TDM_CONFIG, "tdm param");
    check16(buf, 44, 36, "struct afe_param_id_tdm_cfg is 36 bytes");
    check32(buf, 48, AFE_API_VERSION_TDM_CONFIG, "tdm minor version");
    check32(buf, 52, 4, "four slots");
    check32(buf, 56, 48000, "48 kHz");
    check32(buf, 60, 16, "S16_LE as be_hw_params_fixup forces it");
    check16(buf, 64, AFE_LINEAR_PCM_DATA, "linear pcm");
    check16(buf, 66, AFE_PORT_TDM_SHORT_SYNC_SLOT_MODE, "the DBY override: short sync slot mode");
    check16(buf, 68, AFE_PORT_TDM_SYNC_SRC_INTERNAL, "internal sync");
    check16(buf, 70, 4, "four slots per frame");
    check16(buf, 74, 1, "invert sync pulse");
    check16(buf, 76, 1, "sync data delay");
    check16(buf, 78, 32, "32 bit slots");
    check32(buf, 80, 0x0000000Fu, "slot mask for four slots");

    /* Slot mapping v2: four channels at bit offsets 0, 4, 8 and 12, the rest invalid. */
    const uint16_t offsets[4] = {0, 4, 8, 12};
    const AprSlotMap map = {4, 16, 0, 1, offsets};
    n = apr_afe_slot_mapping(buf, sizeof buf, AFE_PORT_IDX_PRIMARY_TDM_RX, AFE_PORT_ID_PRIMARY_TDM_RX, &map);
    check(n == 124, "slot mapping v2 packet is 124 bytes");
    check_header(buf, n, AprSvcAfe, 0, AFE_PORT_IDX_PRIMARY_TDM_RX, APR_OP_AFE_PORT_SET_PARAM_V2);
    check16(buf, 22, 88, "slot mapping v2 payload_size");
    check32(buf, 36, AFE_MODULE_TDM, "tdm module for the slot mapping");
    check32(buf, 40, AFE_PARAM_ID_PORT_SLOT_MAPPING_CONFIG, "slot mapping param");
    check16(buf, 44, 76, "struct afe_param_id_slot_mapping_cfg_v2 is 76 bytes");
    check32(buf, 48, AFE_API_VERSION_SLOT_MAPPING_V2, "minor version 2");
    check16(buf, 52, 4, "four channels");
    check16(buf, 54, 16, "16 bit slots in the mapping record");
    check32(buf, 56, 0, "MSB aligned");
    check16(buf, 60, 0, "slot 1 offset");
    check16(buf, 62, 4, "slot 2 offset");
    check16(buf, 64, 8, "slot 3 offset");
    check16(buf, 66, 12, "slot 4 offset");
    check16(buf, 68, AFE_SLOT_MAPPING_OFFSET_INVALID, "unused offsets are invalid");
    check16(buf, 122, AFE_SLOT_MAPPING_OFFSET_INVALID, "the last of the 32 offsets");

    /* The v1 shape carries eight offsets only. */
    AprSlotMap map1 = {4, 16, 0, 0, offsets};
    n = apr_afe_slot_mapping(buf, sizeof buf, AFE_PORT_IDX_PRIMARY_TDM_RX, AFE_PORT_ID_PRIMARY_TDM_RX, &map1);
    check(n == 76, "slot mapping v1 packet is 76 bytes");
    check16(buf, 22, 40, "slot mapping v1 payload_size");
    check16(buf, 44, 28, "struct afe_param_id_slot_mapping_cfg is 28 bytes");
    check32(buf, 48, AFE_API_VERSION_SLOT_MAPPING_V1, "minor version 1");
    check16(buf, 74, AFE_SLOT_MAPPING_OFFSET_INVALID, "the last of the eight offsets is unused, not a channel");

    check(apr_afe_slot_mapping(buf, sizeof buf, 55, 0x9000, 0) == 0, "a null slot mapping is refused");
    map1.num_channels = 9;
    check(apr_afe_slot_mapping(buf, sizeof buf, 55, 0x9000, &map1) == 0, "more than eight channels in v1 is refused");

    /* Start and stop: a u16 port id plus a u16 reserved, so 24 bytes. */
    n = apr_afe_port_start(buf, sizeof buf, AFE_PORT_IDX_PRIMARY_TDM_RX, AFE_PORT_ID_PRIMARY_TDM_RX);
    check(n == 24, "port start is 24 bytes");
    check_header(buf, n, AprSvcAfe, 0, AFE_PORT_IDX_PRIMARY_TDM_RX, APR_OP_AFE_PORT_START);
    check16(buf, 20, AFE_PORT_ID_PRIMARY_TDM_RX, "started port");
    check16(buf, 22, 0, "start reserved");
    n = apr_afe_port_stop(buf, sizeof buf, AFE_PORT_IDX_PRIMARY_TDM_RX, AFE_PORT_ID_PRIMARY_TDM_RX);
    check(n == 24, "port stop is 24 bytes");
    check32(buf, 16, APR_OP_AFE_PORT_STOP, "stop opcode");

    /* A tiny buffer and an oversized payload are both refused rather than truncated. */
    check(apr_afe_port_start(buf, 23, 55, 0x9000) == 0, "a buffer below the packet size is refused");
    uint8_t big[APR_MAX_PACKET] = {0};
    AprParam too_big = {AFE_MODULE_TDM, AFE_PARAM_ID_PORT_SLOT_MAPPING_CONFIG, 0, big, APR_MAX_PACKET};
    check(apr_afe_port_set_param(buf, sizeof buf, 55, 0x9000, &too_big) == 0, "a payload over the message limit is refused");
}

static void test_adm_asm(void)
{
    uint8_t buf[APR_MAX_PACKET];

    const AprAdmOpen open4 = {AFE_PORT_ID_PRIMARY_TDM_RX, ADM_PATH_PLAYBACK, 4, 16, ADM_TOPOLOGY_NULL, 48000};
    uint32_t n = apr_adm_device_open(buf, sizeof buf, 0x00370000u, &open4);
    check(n == 48, "device open for four channels is 48 bytes");
    check_header(buf, n, AprSvcAdm, AFE_PORT_ID_PRIMARY_TDM_RX, 0x00370000u, APR_OP_ADM_DEVICE_OPEN_V8);
    check16(buf, 20, 0, "legacy device session");
    check16(buf, 22, ADM_PATH_PLAYBACK, "playback path");
    check32(buf, 24, ADM_TOPOLOGY_NULL, "null copp topology");
    check16(buf, 28, AFE_PORT_ID_PRIMARY_TDM_RX, "endpoint 1 is the AFE port");
    check16(buf, 30, ADM_ENDPOINT_ABSENT, "endpoint 2 absent");
    check16(buf, 32, ADM_ENDPOINT_ABSENT, "endpoint 3 absent");
    check16(buf, 34, 0, "PCM, not compressed");
    check16(buf, 36, 4, "four channels");
    check16(buf, 38, 16, "16 bit");
    check32(buf, 40, 48000, "48 kHz");
    check(buf[44] == PCM_CHANNEL_FL && buf[45] == PCM_CHANNEL_FR && buf[46] == PCM_CHANNEL_LS && buf[47] == PCM_CHANNEL_RS,
          "the four channel map is FL FR LS RS");

    const AprAdmOpen open2 = {AFE_PORT_ID_PRIMARY_TDM_RX, ADM_PATH_PLAYBACK, 2, 16, ADM_TOPOLOGY_NULL, 48000};
    n = apr_adm_device_open(buf, sizeof buf, 0x00370000u, &open2);
    check(n == 48, "two channels still rounds the mapping up to four bytes");
    check(buf[44] == PCM_CHANNEL_FL && buf[45] == PCM_CHANNEL_FR && buf[46] == 0 && buf[47] == 0, "the stereo map");

    const AprAdmOpen bad = {AFE_PORT_ID_PRIMARY_TDM_RX, ADM_PATH_PLAYBACK, 0, 16, ADM_TOPOLOGY_NULL, 48000};
    check(apr_adm_device_open(buf, sizeof buf, 1, &bad) == 0, "zero channels is refused");

    const uint16_t copps[1] = {0};
    n = apr_adm_matrix_map(buf, sizeof buf, 0, 1, copps, 1);
    check(n == 36, "matrix map with one copp is 36 bytes, as the vendor sizes it");
    check_header(buf, n, AprSvcAdm, 0, 0, APR_OP_ADM_MATRIX_MAP_V5);
    check32(buf, 20, ADM_MATRIX_ID_AUDIO_RX, "audio rx matrix");
    check32(buf, 24, 1, "one session");
    check16(buf, 28, 1, "session id");
    check16(buf, 30, 1, "one copp");
    check16(buf, 32, 0, "copp id");
    check(buf[34] == 0 && buf[35] == 0, "the vendor leaves two zero bytes per copp in the packet");

    const uint64_t addr = 0x100000;
    const uint32_t size = 4096;
    n = apr_asm_memory_map(buf, sizeof buf, 3, &addr, &size, 1);
    check(n == 40, "mapping one region is 40 bytes");
    check_header(buf, n, AprSvcAsm, 0, 3, APR_OP_ASM_MEM_MAP);
    check16(buf, 20, ADSP_MEMORY_MAP_SHMEM8_4K_POOL, "the 4 KB pool");
    check16(buf, 22, 1, "one region");
    check32(buf, 24, 0, "property flag");
    check32(buf, 28, 0x100000u, "region low word");
    check32(buf, 32, 0, "region high word");
    check32(buf, 36, 4096, "region size");

    n = apr_asm_open_write(buf, sizeof buf, 9, 1, ASM_STREAM_ID_1, 16, ASM_POPP_TOPOLOGY_DEFAULT);
    check(n == 36, "open write is 36 bytes");
    check_header(buf, n, AprSvcAsm, 0x0101, 9, APR_OP_ASM_OPEN_WRITE_V3);
    check32(buf, 20, 0, "legacy stream session");
    check16(buf, 24, ASM_END_POINT_DEVICE_MATRIX, "matrix sink");
    check16(buf, 26, 16, "16 bit");
    check32(buf, 28, ASM_POPP_TOPOLOGY_DEFAULT, "default popp topology");
    check32(buf, 32, ASM_MEDIA_FMT_MULTI_CHANNEL_PCM_V5, "multi channel pcm v5");

    const AprPcmFormat fmt = {4, 16, 16, ASM_Q_FORMAT_DEFAULT, 48000};   /* msm-pcm-q6-v2.c:375-377 for S16_LE */
    n = apr_asm_media_format(buf, sizeof buf, 10, 1, ASM_STREAM_ID_1, &fmt);
    check(n == 72, "the v5 pcm format block is 72 bytes");
    check16(buf, 6, 0x0101, "the session port is session<<8 | stream");
    check32(buf, 20, ASM_FMT_BLOCK_BYTES, "fmt_blk_size");
    check16(buf, 24, 4, "four channels");
    check16(buf, 26, 16, "16 bits per sample");
    check32(buf, 28, 48000, "48 kHz");
    check16(buf, 32, 1, "signed pcm");
    check16(buf, 34, 16, "sixteen bit sample word");
    check16(buf, 36, ASM_LITTLE_ENDIAN, "little endian");
    check16(buf, 38, ASM_Q_FORMAT_DEFAULT, "default Q format");
    check(buf[40] == PCM_CHANNEL_FL && buf[43] == PCM_CHANNEL_RS, "the format map matches the COPP map");
    check(buf[71] == 0, "the mapping array is zero padded");

    n = apr_asm_run(buf, sizeof buf, 11, 1, ASM_STREAM_ID_1, 0, 0);
    check(n == 32, "run v2 is 32 bytes");
    check32(buf, 16, APR_OP_ASM_RUN_V2, "run opcode");

    /* The soft volume, in band: the zeroed memory header, the payload size, then the 12-byte v1 parameter header with
     * struct asm_volume_ctrl_master_gain behind it - 20 + 12 + 4 + 12 + 4 bytes, __q6asm_set_volume's packet. */
    n = apr_asm_set_volume(buf, sizeof buf, 11, 1, ASM_STREAM_ID_1, ASM_Q13_UNITY);
    check(n == 52, "in-band master gain is 52 bytes with the v1 header");
    check_header(buf, n, AprSvcAsm, 0x0101, 11, APR_OP_ASM_SET_PP_PARAMS_V2);
    for (uint32_t i = 20; i < 32; ++i) check(buf[i] == 0, "the in-band memory header is zero in the asm packet too");
    check32(buf, 32, 16, "payload_size is the v1 header plus the gain struct");
    check32(buf, 36, ASM_MODULE_ID_VOL_CTRL, "VOL_CTRL module");
    check32(buf, 40, ASM_PARAM_ID_VOL_CTRL_MASTER_GAIN, "MASTER_GAIN param");
    check16(buf, 44, 4, "struct asm_volume_ctrl_master_gain is 4 bytes");
    check16(buf, 46, 0, "gain packet v1 reserved");
    check16(buf, 48, ASM_Q13_UNITY, "Q13 unity is 16384, the 0 dB of the vendor volume table");
    check16(buf, 50, 0, "the gain struct's own reserved field is zero");
    apr_set_iid_supported(1);
    n = apr_asm_set_volume(buf, sizeof buf, 11, 1, ASM_STREAM_ID_1, 8192);
    check(n == 56, "in-band master gain is 56 bytes with the v3 header");
    check_header(buf, n, AprSvcAsm, 0x0101, 11, APR_OP_ASM_SET_PP_PARAMS_V3);
    check32(buf, 32, 20, "payload_size covers the v3 header and the gain struct");
    check32(buf, 36, ASM_MODULE_ID_VOL_CTRL, "v3 gain module");
    check16(buf, 40, 0, "q6asm_set_soft_volume_module_instance_ids uses INSTANCE_ID_0");
    check32(buf, 44, ASM_PARAM_ID_VOL_CTRL_MASTER_GAIN, "v3 gain param id");
    check32(buf, 48, 4, "v3 gain param size");
    check16(buf, 52, 8192, "half scale is 8192 in Q13");
    apr_set_iid_supported(0);
    check(apr_asm_set_volume(buf, 51, 11, 1, ASM_STREAM_ID_1, ASM_Q13_UNITY) == 0, "a gain packet in a short buffer is refused");

    n = apr_asm_write(buf, sizeof buf, 12, 1, ASM_STREAM_ID_1, 0x100000, 0xABCD, 2048, 5, 0, 0);
    check(n == 52, "write v2 is 52 bytes");
    check32(buf, 20, 0x100000u, "buffer low word");
    check32(buf, 24, 0, "buffer high word");
    check32(buf, 28, 0xABCD, "memory map handle");
    check32(buf, 32, 2048, "buffer size");
    check32(buf, 36, 5, "sequence id");
    check32(buf, 48, 0, "flags");

    n = apr_asm_stream_close(buf, sizeof buf, 13, 1, ASM_STREAM_ID_1);
    check(n == 20, "stream close is header only");
    check32(buf, 16, APR_OP_ASM_STREAM_CLOSE, "close opcode");

    /* The vendor masks the region count into one byte (num_regions = bufcnt & 0x00ff); more than that would describe a
     * different buffer list than the one written, so the encoder refuses it instead. */
    uint64_t many_addr[260];
    uint32_t many_size[260];
    for (uint32_t i = 0; i < 260; ++i) {many_addr[i] = 0x100000 + i * 4096; many_size[i] = 4096;}
    check(apr_asm_memory_map(buf, sizeof buf, 3, many_addr, many_size, 260) == 0, "more than 255 regions is refused");

    /* q6asm_update_token (q6asm.c:61-69): stream, session and buffer index in the low three bytes, direction and the
     * no-wait flag as the two bits of the fourth. */
    check(apr_asm_token(1, ASM_STREAM_ID_1, 0, 0) == 0x00000101u, "a command token is stream 1 of session 1");
    check(apr_asm_token(1, ASM_STREAM_ID_1, 2, ASM_TOKEN_NO_WAIT) == 0x02020101u, "the buffer index and no-wait ride in the write token");
    check(apr_asm_token(1, ASM_STREAM_ID_1, 2, ASM_TOKEN_DIR_OUT | ASM_TOKEN_NO_WAIT) == 0x03020101u, "the direction flag is bit 24");
    check(apr_asm_token_buf_index(0x02020101u) == 2, "the buffer index reads back out of the token");
}

/* The three answers the playback ladder cannot run without: the COPP the ADM picked, the handle the ASM gave the mapped ring,
 * and the write-done that frees one buffer. All three are read out of the packet the way q6adm.c/q6asm.c read them. */
static void test_asm_answers(void)
{
    uint8_t buf[APR_MAX_PACKET];
    AprMessage m;

    memset(buf, 0, sizeof buf);
    put_le16(buf, 0, APR_HDR_FIELD(AprMsgCmdRsp, APR_HEADER_BYTES, APR_PKT_VER));
    put_le16(buf, 2, 28);
    buf[4] = AprSvcAdm; buf[5] = AprDomainADSP;
    buf[8] = AprSvcAdm; buf[9] = AprDomainAPPS;
    put_le32(buf, 12, 0x00370000u);
    put_le32(buf, 16, APR_OP_ADM_DEVICE_OPEN_RSP_V8);
    put_le32(buf, APR_HEADER_BYTES, 0);                    /* status */
    put_le16(buf, APR_HEADER_BYTES + 4, 55);               /* copp_id the DSP assigned */
    check(apr_decode(buf, 28, &m) == AprOk, "decode the device open answer");
    uint32_t status = 0;
    uint16_t copp = 0;
    check(apr_adm_device_open_rsp(&m, &status, &copp) == 0 && status == 0 && copp == 55, "the open answer gives back the COPP id");
    put_le32(buf, APR_HEADER_BYTES, 5);                    /* a rejected open */
    check(apr_adm_device_open_rsp(&m, &status, &copp) == 0 && status == 5, "and the DSP status with it");
    put_le32(buf, 16, APR_OP_AFE_PORT_START);
    check(apr_decode(buf, 28, &m) == AprOk && apr_adm_device_open_rsp(&m, &status, &copp) == -1,
          "another opcode is not a device open answer");

    memset(buf, 0, APR_HEADER_BYTES);
    put_le16(buf, 0, APR_HDR_FIELD(AprMsgCmdRsp, APR_HEADER_BYTES, APR_PKT_VER));
    put_le16(buf, 2, 24);
    put_le32(buf, 12, 7);
    put_le32(buf, 16, APR_OP_ASM_MEM_MAP_RSP);
    put_le32(buf, APR_HEADER_BYTES, 0x1122);
    check(apr_decode(buf, 24, &m) == AprOk, "decode the memory map answer");
    uint32_t handle = 0;
    check(apr_asm_mem_map_rsp(&m, &handle) == 0 && handle == 0x1122, "the handle is the first payload word (q6asm.c:1809)");
    put_le32(buf, APR_HEADER_BYTES, 0);
    check(apr_asm_mem_map_rsp(&m, &handle) == -1, "a zero handle is not a mapping (the driver only wakes on a non-zero one)");

    memset(buf, 0, APR_HEADER_BYTES);
    put_le16(buf, 0, APR_HDR_FIELD(AprMsgEvent, APR_HEADER_BYTES, APR_PKT_VER));
    put_le16(buf, 2, 28);
    put_le32(buf, 12, 0x02020101u);                        /* buffer index 2, session 1, stream 1, no-wait */
    put_le32(buf, 16, APR_OP_ASM_WRITE_DONE_V2);
    put_le32(buf, APR_HEADER_BYTES, 0x100000u);
    put_le32(buf, APR_HEADER_BYTES + 4, 0);
    check(apr_decode(buf, 28, &m) == AprOk, "decode a write done");
    uint8_t buffer = 0;
    check(apr_asm_write_done(&m, &buffer, &status) == 0 && buffer == 2 && status == 0,
          "the completed buffer comes from the token, not the payload (q6asm.c:2203)");
    put_le32(buf, APR_HEADER_BYTES + 4, 9);
    check(apr_asm_write_done(&m, &buffer, &status) == 0 && status == 9, "and the payload carries its status");
    put_le16(buf, 2, 24);                                  /* only one payload word */
    check(apr_decode(buf, 24, &m) == AprOk && apr_asm_write_done(&m, &buffer, &status) == -1,
          "a write done below two payload words is refused (q6asm.c:2186)");
}

static void test_decode_and_pending(void)
{
    uint8_t buf[APR_MAX_PACKET];
    AprMessage m;

    /* An AFE port start answer: command response, token 55. */
    memset(buf, 0, sizeof buf);
    put_le16(buf, 0, APR_HDR_FIELD(AprMsgCmdRsp, APR_HEADER_BYTES, APR_PKT_VER));
    put_le16(buf, 2, 24);
    buf[4] = AprSvcAfe; buf[5] = AprDomainADSP;
    buf[8] = AprSvcAfe; buf[9] = AprDomainAPPS;
    put_le16(buf, 6, 0); put_le16(buf, 10, 0);
    put_le32(buf, 12, 55);
    put_le32(buf, 16, APR_OP_AFE_PORT_START);
    check(apr_decode(buf, 24, &m) == AprOk, "decodes a command response");
    check(m.msg_type == AprMsgCmdRsp && m.src_domain == AprDomainADSP && m.dest_domain == AprDomainAPPS, "domains read back");
    check(m.token == 55 && m.payload == buf + APR_HEADER_BYTES && m.payload_bytes == 4, "payload starts after the header");

    /* A packet whose declared size exceeds what the channel delivered must not be trusted. */
    check(apr_decode(buf, 23, &m) == AprBadSize, "a short read is refused");
    check(apr_decode(buf, 19, &m) == AprShort, "less than a header is refused");
    put_le16(buf, 0, APR_HDR_FIELD(AprMsgCmdRsp, 24, APR_PKT_VER));
    check(apr_decode(buf, 24, &m) == AprOk, "a 24 byte header is legal");
    check(m.header_bytes == 24 && m.payload_bytes == 0, "payload starts after a long header");
    put_le16(buf, 0, APR_HDR_FIELD(AprMsgCmdRsp, 16, APR_PKT_VER));
    check(apr_decode(buf, 24, &m) == AprBadHeader, "a header below 20 bytes is refused");
    put_le16(buf, 0, APR_HDR_FIELD(AprMsgCmdRsp, APR_HEADER_BYTES, 3));
    check(apr_decode(buf, 24, &m) == AprBadVersion, "a version above the two the vendor accepts is refused");

    /* apr.c:613-640 refuses a version only when it is above APR_PKT_VER + 1, so the ADSP is allowed to answer in version 1.
     * The board's last boot logged nine replies and nine refusals; a reply we cannot open is a reply we never received. */
    put_le16(buf, 0, APR_HDR_FIELD(AprMsgCmdRsp, APR_HEADER_BYTES, APR_PKT_VER + 1));
    check(apr_decode(buf, 24, &m) == AprOk, "the newer of the two legal versions decodes");
    check(m.token == 55 && m.opcode == APR_OP_AFE_PORT_START, "and its fields read back unchanged");

    /* "Wrong paket size" and "Wrong hdr size" are the vendor's two different rules; they get two different codes here so the boot
     * log says which one stopped a message instead of making a refusal and a silence look the same. */
    put_le16(buf, 0, APR_HDR_FIELD(AprMsgCmdRsp, APR_HEADER_BYTES, APR_PKT_VER));
    put_le16(buf, 2, 16);
    check(apr_decode(buf, 20, &m) == AprBadSize, "a packet size below the fixed header is refused as a size");
    put_le16(buf, 0, APR_HDR_FIELD(AprMsgCmdRsp, 24, APR_PKT_VER));
    put_le16(buf, 2, 22);
    check(apr_decode(buf, 24, &m) == AprBadSize, "a packet smaller than its own header is refused as a size");

    /* The pending table: token match, response opcode match, basic result, event, timeout. */
    AprPending p;
    apr_pending_init(&p);
    const uint32_t t_start = apr_pending_add(&p, APR_OP_AFE_PORT_START, APR_OP_AFE_PORT_START);
    const uint32_t t_clk = apr_pending_add(&p, APR_OP_AFE_SVC_SET_PARAM, APR_OP_BASIC_RESULT);
    const uint32_t t_write = apr_pending_add(&p, APR_OP_ASM_WRITE_V2, APR_OP_ASM_WRITE_DONE_V2);
    check(t_start == 1 && t_clk == 2 && t_write == 3, "tokens are handed out in order");
    check(apr_pending_waiting(&p) == 3, "three requests are waiting");

    memset(buf, 0, APR_HEADER_BYTES);
    put_le16(buf, 0, APR_HDR_FIELD(AprMsgCmdRsp, APR_HEADER_BYTES, APR_PKT_VER));
    put_le16(buf, 2, 20);
    put_le32(buf, 12, t_start);
    put_le32(buf, 16, APR_OP_AFE_PORT_START);
    check(apr_decode(buf, 20, &m) == AprOk, "decode start answer");
    uint32_t request = 0, status = 0;
    check(apr_pending_reply(&p, &m, &request, &status) == AprPAck, "the start answer closes its request");
    check(request == APR_OP_AFE_PORT_START, "the request opcode is reported back");

    /* A write completion arrives as an event, with the write's token. */
    put_le16(buf, 0, APR_HDR_FIELD(AprMsgEvent, APR_HEADER_BYTES, APR_PKT_VER));
    put_le16(buf, 2, 36);
    put_le32(buf, 12, t_write);
    put_le32(buf, 16, APR_OP_ASM_WRITE_DONE_V2);
    put_le32(buf, APR_HEADER_BYTES, 0x100000u);
    check(apr_decode(buf, 36, &m) == AprOk, "decode write done");
    check(apr_pending_reply(&p, &m, &request, &status) == AprPAck, "the write completion closes the write");
    check(request == APR_OP_ASM_WRITE_V2, "and names the write as its request");

    /* A basic result names the request it answers and carries its status. */
    put_le16(buf, 0, APR_HDR_FIELD(AprMsgCmdRsp, APR_HEADER_BYTES, APR_PKT_VER));
    put_le16(buf, 2, 28);
    put_le32(buf, 12, 999);                                   /* a token we never used: the opcode inside must match */
    put_le32(buf, 16, APR_OP_BASIC_RESULT);
    put_le32(buf, APR_HEADER_BYTES, APR_OP_AFE_SVC_SET_PARAM);
    put_le32(buf, APR_HEADER_BYTES + 4, 2);
    check(apr_decode(buf, 28, &m) == AprOk, "decode basic result");
    check(apr_pending_reply(&p, &m, &request, &status) == AprPResult, "a basic result closes its request");
    check(request == APR_OP_AFE_SVC_SET_PARAM && status == 2, "and reports the DSP status");
    check(p.errors == 1 && p.answers == 2, "a non-zero status is the error q6afe.c:1090 says it is");

    /* The same command answered with status 0. q6afe.c:1080-1097 treats only payload[1] != 0 as a failure, so a zero
     * is the DSP accepting the command and has to be counted as an answer; the board's clock gate answers exactly this. */
    const uint32_t answers0 = p.answers, errors0 = p.errors;
    const uint32_t t_gain = apr_pending_add(&p, APR_OP_AFE_SVC_SET_PARAM, APR_OP_BASIC_RESULT);
    put_le32(buf, APR_HEADER_BYTES + 4, 0);
    check(apr_decode(buf, 28, &m) == AprOk, "decode the accepted basic result");
    check(apr_pending_reply(&p, &m, &request, &status) == AprPResult, "a zero status still closes the request");
    check(t_gain != 0 && status == 0 && p.errors == errors0 && p.answers == answers0 + 1,
          "and lands in the answers, not the errors");

    /* Something for nobody: counted, slot count unchanged. */
    put_le32(buf, 12, 4242);
    put_le32(buf, 16, 0xDEADBEEFu);
    check(apr_decode(buf, 28, &m) == AprOk, "decode stranger");
    check(apr_pending_reply(&p, &m, &request, &status) == AprPUnknown, "an unmatched packet is not an answer");
    check(p.unmatched == 1, "the unmatched counter moved");

    /* Every slot frees, and the table can be refilled. */
    check(apr_pending_timeout(&p, t_write) == 0, "a closed request does not time out");
    check(apr_pending_waiting(&p) == 0, "nothing is left waiting");
    for (uint32_t i = 0; i < APR_PENDING_SLOTS; ++i) check(apr_pending_add(&p, 1, 1) != 0, "the table refills");
    check(apr_pending_add(&p, 1, 1) == 0, "and refuses a ninth request");
}

int main(void)
{
    test_core();
    test_afe();
    test_adm_asm();
    test_asm_answers();
    test_decode_and_pending();
    if (failures) {printf("%d check(s) failed\n", failures); return 1;}
    printf("PASS apr: core handshake, AFE v1/v2 parameters, TDM ladder, ADM route, ASM stream, answers, decoder\n");
    return 0;
}
