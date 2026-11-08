/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The legacy (pre-AudioReach) audio packet router on this SoC. The Kona vendor stack drives the DSP's audio services over one GLINK
 * channel, apr_audio_svc, granted 0x200 byte intents (arch/arm64/boot/dts/vendor/qcom/kona.dtsi:4191-4216), and every request is a
 * 20-byte header plus a service specific payload (techpack/audio/include/ipc/apr.h:24-56). This module only encodes requests and
 * decodes answers: no I/O, no waits, no allocation. The owner (package/DbyAudio.cpp) sends the bytes over the channel, feeds back
 * what arrives, and runs the retry timers; the pending table matches an answer to its request by token, by the response opcode the
 * vendor waits for, or (for a basic result) by the request opcode the DSP echoes inside the payload.
 *
 * The parameter packets come in two families and the vendor picks between them at send time from one flag,
 * q6common_is_instance_id_supported() (dsp/q6common.c:32, set only by the "MSM Instance ID Support" mixer control in
 * asoc/msm-pcm-routing-v2.c:30981). Nothing flips that control on this board, so the default - and what the ADSP of this unit is
 * used to - is the v1/v2 family: param_hdr_v1 (12 bytes) with AFE_PORT_CMD_SET_PARAM_V2 and AFE_SVC_CMD_SET_PARAM. The v3 family is
 * encoded too, because the flag is runtime state, but it is not what we send.
 *
 * Nothing here is invented: every opcode, field order and value is transcribed from this unit's own vendor tree. */
#ifndef SHENG_APR_H
#define SHENG_APR_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define APR_HEADER_BYTES 20u
#define APR_MAX_PACKET 512u            /* the intent size this channel is granted, and the reference API's message limit */

/* Header fields: message type 15:8, header size in words 7:4, packet version 3:0 (apr.h:37-56). */
#define APR_HDR_FIELD(msg_type, header_bytes, version) \
    (uint16_t)((((msg_type) & 3u) << 8) | ((((header_bytes) / 4u) & 0xfu) << 4) | ((version) & 0xfu))

enum {AprMsgEvent = 0, AprMsgCmdRsp = 1, AprMsgSeqCmd = 2, AprMsgNSeqCmd = 3};
#define APR_PKT_VER 0u

/* Domains and service ids (apr.h:58-77). apr_send_pkt (apr.c:394-398) forces src_svc = dest_svc = the registered service and
 * src_domain = APPS, dest_domain = ADSP, so a request carries one service id in both slots. */
enum {AprDomainADSP = 4, AprDomainAPPS = 5};
enum {AprSvcAdspCore = 3, AprSvcAfe = 4, AprSvcAsm = 7, AprSvcAdm = 8};

/* Sizes of the pieces a payload is built from. */
#define APR_PARAM_HDR_V1_BYTES 12u     /* struct param_hdr_v1: u32 module_id, u32 param_id, u16 param_size, u16 reserved */
#define APR_PARAM_HDR_V3_BYTES 16u     /* struct param_hdr_v3: u32 module_id, u16 instance, u16 reserved, u32 param_id, u32 size */
#define APR_MEM_HDR_BYTES 12u          /* struct mem_mapping_hdr: three u32, all zero when the parameter travels in band */

/* The APR accept/refuse answer every service may return: payload {request opcode, status} (apr.h:51-56). The status is an
 * error only when it is not zero (dsp/q6afe.c:1080-1097), so there is no "accepted" constant to compare it against. */
#define APR_OP_BASIC_RESULT 0x000110E8u

/* AVCS, the core service (dsp/q6core.c). The state query is header only and its answer carries one u32; the framework version query
 * is header only, ports 0 and token 0 (q6core.c:539-556), and its answer is struct avcs_get_fwk_version plus one
 * struct avs_svc_api_info per service (include/dsp/apr_audio-v2.h:11668-11704). */
#define APR_OP_CORE_GET_STATE 0x0001290Cu   /* AVCS_CMD_ADSP_EVENT_GET_STATE (include/dsp/q6core.h:13) */
#define APR_OP_CORE_STATE_RSP 0x0001290Du   /* AVCS_CMDRSP_ADSP_EVENT_GET_STATE (include/dsp/q6core.h:14) */
#define APR_OP_AVCSS_GET_FWK_VERSION 0x0001292Cu
#define APR_OP_AVCSS_GET_FWK_VERSION_RSP 0x0001292Du
#define APR_AVCS_SERVICE_API_MAX 25u        /* VSS_MAX_AVCS_NUM_SERVICES (apr_audio-v2.h:10566) */

/* The service ids whose API version picks the wire family of a later command (include/dsp/q6core.h:17, include/dsp/q6afe-v2.h:43). */
#define APRV2_IDS_SERVICE_ID_ADSP_CORE_V 3u
#define APRV2_IDS_SERVICE_ID_ADSP_AFE_V 4u
#define APRV2_IDS_SERVICE_ID_ADSP_ASM_V 7u
#define APRV2_IDS_SERVICE_ID_ADSP_ADM_V 8u
#define AFE_API_VERSION_V3 3u

/* AFE (dsp/q6afe.c). In-band parameter set, in both families, then port start/stop. */
#define APR_OP_AFE_SVC_SET_PARAM 0x000100F3u        /* AFE_SVC_CMD_SET_PARAM (apr_audio-v2.h:11776) */
#define APR_OP_AFE_SVC_SET_PARAM_V2 0x000100FCu     /* AFE_SVC_CMD_SET_PARAM_V2 (apr_audio-v2.h:11777) */
#define APR_OP_AFE_PORT_SET_PARAM_V2 0x000100EFu    /* AFE_PORT_CMD_SET_PARAM_V2 (apr_audio-v2.h:2028) */
#define APR_OP_AFE_PORT_SET_PARAM_V3 0x000100FAu    /* AFE_PORT_CMD_SET_PARAM_V3 (apr_audio-v2.h:2050) */
#define APR_OP_AFE_PORT_START 0x000100E5u           /* AFE_PORT_CMD_DEVICE_START (apr_audio-v2.h:5133) */
#define APR_OP_AFE_PORT_STOP 0x000100E6u            /* AFE_PORT_CMD_DEVICE_STOP (apr_audio-v2.h:5150) */
#define APR_OP_AFE_HW_VOTE_REQUEST 0x000100F4u
#define APR_OP_AFE_HW_VOTE_RESPONSE 0x000100F5u
#define APR_OP_AFE_HW_VOTE_DROP 0x000100F6u

#define AFE_CLK_TOKEN 1024u                          /* the token the clock packet is always sent with (q6afe.c:62) */
#define AFE_MODULE_CLOCK_SET 0x0001028Fu             /* apr_audio-v2.h:12206 */
#define AFE_PARAM_ID_CLOCK_SET 0x00010290u           /* apr_audio-v2.h:12207 */
#define AFE_MODULE_TDM 0x0001028Au                   /* apr_audio-v2.h:3777 */
#define AFE_PARAM_ID_PORT_SLOT_MAPPING_CONFIG 0x00010297u  /* apr_audio-v2.h:3782 */
#define AFE_API_VERSION_SLOT_MAPPING_V1 1u           /* AFE_API_VERSION_SLOT_MAPPING_CONFIG (apr_audio-v2.h:3787) */
#define AFE_API_VERSION_SLOT_MAPPING_V2 2u           /* AFE_API_VERSION_SLOT_MAPPING_CONFIG_V2 (apr_audio-v2.h:3792) */
#define AFE_SLOT_MAPPING_OFFSET_INVALID 0xFFFFu
#define AFE_PORT_MAX_AUDIO_CHAN_V2 32u               /* AFE_PORT_MAX_AUDIO_CHAN_CNT_V2 (apr_audio-v2.h:3329) */
#define AFE_MODULE_AUDIO_DEV_INTERFACE 0x0001020Cu
#define AFE_PARAM_ID_TDM_CONFIG 0x0001029Du
#define AFE_API_VERSION_TDM_CONFIG 1u
#define AFE_CLK_API_VERSION 1u                       /* AFE_API_VERSION_CLOCK_SET (asoc/msm-dai-q6-v2.c:42) */
#define AFE_LINEAR_PCM_DATA 0x0000u
#define AFE_PORT_TDM_SHORT_SYNC_SLOT_MODE 2u
#define AFE_PORT_TDM_SYNC_SRC_INTERNAL 1u
#define AFE_CLK_ATTR_INVERT_COUPLE_NO 0x4u           /* Q6AFE_LPASS_CLK_ATTRIBUTE_INVERT_COUPLE_NO */
#define AFE_CLK_ID_PRI_TDM_IBIT 0x200u               /* Q6AFE_LPASS_CLK_ID_PRI_TDM_IBIT: the internal bit clock of Primary TDM */
#define AFE_CLK_ROOT_DEFAULT 0u
#define AFE_PORT_ID_PRIMARY_TDM_RX 0x9000u           /* the DBY speaker backend: 4 slots on Primary TDM (asoc/kona.c:6678-6692) */
#define AFE_PORT_ID_PRIMARY_TDM_TX 0x9001u
#define AFE_PORT_IDX_PRIMARY_TDM_RX 55u              /* IDX_AFE_PORT_ID_PRIMARY_TDM_RX_0 (include/dsp/q6afe-v2.h:121) */
#define AFE_TDM_CLK_HZ 6144000u                      /* 4 slots x 32 bit at 48 kHz (msm-audio-lpass.dtsi:537-554) */

/* ADM (dsp/q6adm.c): one playback COPP per device, then the matrix route that binds it to an ASM session. */
#define APR_OP_ADM_DEVICE_OPEN_V8 0x0001036Au
#define APR_OP_ADM_DEVICE_OPEN_RSP_V8 0x0001036Bu
#define APR_OP_ADM_MATRIX_MAP_V5 0x00010325u
#define ADM_MATRIX_ID_AUDIO_RX 0u
#define ADM_PATH_PLAYBACK 0x1u
#define ADM_MAX_COPPS 5u
#define ADM_ENDPOINT_ABSENT 0xFFFFu                  /* endpoint_id_2/3 for an audio COPP (q6adm.c:3362-3365) */
#define ADM_TOPOLOGY_NULL 0x00010312u
#define ADM_TOPOLOGY_DEFAULT 0x00010314u

/* ASM (dsp/q6asm.c): a PCM stream written through mapped buffers; the samples never ride in the packet. Every ASM command carries
 * src_port = dest_port = (session << 8) | stream_id (__q6asm_add_hdr, q6asm.c:2747-2748) and q6asm_add_hdr passes ac->stream_id, so
 * even the "session level" run command uses the same ports. The memory map is the exception: ports 0 (q6asm_add_mmaphdr, :2859). */
#define APR_OP_ASM_OPEN_WRITE_V3 0x00010DB3u    /* ASM_STREAM_CMD_OPEN_WRITE_V3 (apr_audio-v2.h:7837) */
#define APR_OP_ASM_MEDIA_FMT_V2 0x00010D98u     /* ASM_DATA_CMD_MEDIA_FMT_UPDATE_V2 (:5913) */
#define APR_OP_ASM_MEM_MAP 0x00010D92u          /* ASM_CMD_SHARED_MEM_MAP_REGIONS (:7621) */
#define APR_OP_ASM_MEM_MAP_RSP 0x00010D93u
#define APR_OP_ASM_RUN_V2 0x00010DAAu           /* ASM_SESSION_CMD_RUN_V2 (:7634), the packet q6asm_run sends (:4627) */
#define APR_OP_ASM_WRITE_V2 0x00010DABu         /* ASM_DATA_CMD_WRITE_V2 (:7212) */
#define APR_OP_ASM_WRITE_DONE_V2 0x00010D99u    /* ASM_DATA_EVENT_WRITE_DONE_V2 (:7350) */
#define APR_OP_ASM_STREAM_CLOSE 0x00010BCDu     /* ASM_STREAM_CMD_CLOSE (:8352) */
#define APR_OP_ASM_SET_PP_PARAMS_V2 0x00010DA1u /* ASM_STREAM_CMD_SET_PP_PARAMS_V2 (apr_audio-v2.h:8357). q6asm_set_pp_params
                                                 * picks this opcode when the ADSP does not carry instance ids (dsp/q6asm.c:2932). */
#define APR_OP_ASM_SET_PP_PARAMS_V3 0x0001320Du /* ASM_STREAM_CMD_SET_PP_PARAMS_V3 (:8358): the same packet, with a v3 parameter
                                                 * header and the q6common_is_instance_id_supported() opcode (:2928-2932). */
/* The in-band parameter block of that command: struct asm_stream_cmd_set_pp_params (apr_audio-v2.h:8366-8382) is the memory
 * header, then payload_size, then the pre-packed {parameter header, parameter} - q6asm_pack_and_set_pp_param_in_band feeds it
 * through q6common_pack_pp_params (dsp/q6common.c:54-110) and q6asm_set_pp_params copies it after the zeroed memory header. */
#define ASM_MODULE_ID_VOL_CTRL 0x00010BFEu               /* apr_audio-v2.h:9761, the legacy session's soft volume block */
#define ASM_PARAM_ID_VOL_CTRL_MASTER_GAIN 0x00010BFFu    /* apr_audio-v2.h:9773 */
#define ASM_Q13_UNITY 16384u                              /* struct asm_volume_ctrl_master_gain (:9833-9839): one u16 linear Q13
                                                           * gain, 0 mutes and 16384 is 0 dB, plus a u16 reserved the client must
                                                           * zero. __q6asm_set_volume (dsp/q6asm.c:8803-8828) sends exactly
                                                           * these four bytes to module VOL_CTRL, instance INSTANCE_ID_0. */
#define ASM_MEDIA_FMT_MULTI_CHANNEL_PCM_V5 0x00013222u   /* this board's own choice: msm-pcm-q6-v2.c:394-398 calls
                                                          * q6asm_open_write_v5 once the AVCS ASM api version reaches
                                                          * ADSP_ASM_API_VERSION_V2, and that hands PCM_MEDIA_FORMAT_V5 to
                                                          * __q6asm_open_write (q6asm.c:3709-3714), which maps it to this id
                                                          * through q6asm_get_pcm_format_id (:181-182). */
#define ASM_POPP_TOPOLOGY_DEFAULT 0x00010BE4u        /* DEFAULT_POPP_TOPOLOGY (apr_audio-v2.h:5461). For a legacy PCM session
                                                      * __q6asm_open_write sends cal_info.topology_id, and
                                                      * q6asm_get_asm_topology_apptype sets exactly this value before it looks at
                                                      * any calibration block (q6asm.c:11089-11092, used at :3516-3517), so a
                                                      * postprocopo_id of 0 was never what this ADSP saw.
                                                      * ASM_STREAM_POSTPROCOPO_ID_NONE (0x00010C68) is only substituted when
                                                      * perf_mode is not LEGACY_PCM_MODE (:3519-3520). */
#define ADSP_MEMORY_MAP_SHMEM8_4K_POOL 3u
#define ASM_END_POINT_DEVICE_MATRIX 0u
#define ASM_LEGACY_STREAM_SESSION 0u
#define ASM_STREAM_ID_1 1u                           /* ac->stream_id (q6asm.c:1353) */
#define ASM_LITTLE_ENDIAN 0u                         /* the endianness argument of the format call, msm-pcm-q6-v2.c:449-451 */
#define ASM_Q_FORMAT_DEFAULT 0u                      /* DEFAULT_QF, the mode argument of that same call */
#define ASM_FMT_BLOCK_BYTES 48u                      /* __q6asm_media_format_block_multi_ch_pcm_v5 (q6asm.c:7016-7060) writes
                                                      * sizeof(fmt) - sizeof(fmt.hdr) - sizeof(fmt.fmt_blk): the v5 param block is
                                                      * 2+2+4+2+2+2+2+32 (apr_audio-v2.h:6120-6168, :6192-6197). */

/* The token an ASM packet carries is not a plain counter (q6asm.c:61-69): byte 0 stream id, byte 1 session id, byte 2 buffer
 * index, byte 3 flags with bit 0 = direction (IN/OUT) and bit 1 = no-wait. The write path puts the buffer index here and the
 * write-done event is attributed to a buffer by reading it back, not from the payload (:2203). */
#define ASM_TOKEN_DIR_OUT 1u                         /* ASM_DIRECTION_OFFSET */
#define ASM_TOKEN_NO_WAIT 2u                         /* ASM_CMD_NO_WAIT_OFFSET */
uint32_t apr_asm_token(uint8_t session, uint8_t stream_id, uint8_t buf_index, uint32_t flags);
uint8_t apr_asm_token_buf_index(uint32_t token);
uint8_t apr_asm_token_stream(uint32_t token);

/* Channel ids (apr_audio-v2.h:5757-5835). */
#define PCM_CHANNEL_FL 1u
#define PCM_CHANNEL_FR 2u
#define PCM_CHANNEL_FC 3u
#define PCM_CHANNEL_LS 4u
#define PCM_CHANNEL_RS 5u
#define PCM_CHANNEL_LFE 6u
#define PCM_CHANNEL_CS 7u
#define PCM_CHANNEL_LB 8u
#define PCM_CHANNEL_RB 9u
#define PCM_CHANNEL_TFL 20u
#define PCM_CHANNEL_TFR 21u
#define PCM_CHANNEL_TSL 22u
#define PCM_CHANNEL_TSR 23u
#define PCM_MAX_CHANNELS 32u                         /* PCM_FORMAT_MAX_NUM_CHANNEL_V8, the width of a mapping array */

typedef struct {
    uint8_t msg_type;                                /* AprMsg* */
    uint8_t src_svc, src_domain;
    uint16_t src_port;
    uint8_t dest_svc, dest_domain;
    uint16_t dest_port;
    uint32_t token, opcode;
    const uint8_t *payload;
    uint32_t payload_bytes;
    uint32_t header_bytes, size;
} AprMessage;

enum {AprOk = 0, AprShort = -1, AprBadVersion = -2, AprBadHeader = -3, AprBadSize = -4};

/* Writes the 20-byte header and the payload into out; returns the packet size, 0 when it does not fit or is over the message limit. */
uint32_t apr_encode(uint8_t *out, uint32_t capacity, uint8_t service, uint16_t port, uint32_t token, uint32_t opcode,
                    const void *payload, uint32_t payload_bytes);
/* Parses a received packet (length: the bytes received; the packet may be shorter). Returns AprOk or an error. */
int apr_decode(const uint8_t *in, uint32_t length, AprMessage *m);
/* A basic result's {request opcode, status}; returns 0 when the packet is one. */
int apr_basic_result(const AprMessage *m, uint32_t *opcode, uint32_t *status);

/* The one flag that chooses the wire family of every parameter packet, mirroring the vendor's q6common_ctl. Zero (the default) is
 * the family this ADSP is configured for. */
void apr_set_iid_supported(int supported);
int apr_iid_supported(void);

/* --- requests, one function per vendor command, in the order the driver sends them --- */

/* AVCS_CMD_ADSP_EVENT_GET_STATE: header only, ports 0 (q6core.c:1112-1130). */
uint32_t apr_core_get_state(uint8_t *out, uint32_t capacity, uint32_t token);
/* AVCS_CMD_GET_FWK_VERSION: header only, ports 0 and token 0 (q6core.c:539-556). */
uint32_t apr_avcs_get_fwk_version(uint8_t *out, uint32_t capacity);
/* Reads one service's API version out of an AVCS_CMDRSP_GET_FWK_VERSION payload; num_services reports what the DSP listed. */
int apr_avcs_service_api(const AprMessage *m, uint32_t service_id, uint32_t *api_version, uint32_t *num_services);

/* One in-band parameter: module/param ids plus the parameter struct that follows the parameter header. */
typedef struct {
    uint32_t module_id, param_id;
    uint16_t instance_id;                            /* 0; only carried by the v3 header */
    const void *data;
    uint32_t data_bytes;
} AprParam;

/* AFE SVC-level set-param, the clock's path: q6afe_clk_set_params_v1 puts payload_size before the zeroed memory header, the v2
 * struct puts it after. Ports 0; the clock packet carries token AFE_CLK_TOKEN. */
uint32_t apr_afe_svc_set_param(uint8_t *out, uint32_t capacity, uint32_t token, const AprParam *param);
/* struct afe_clk_set (apr_audio-v2.h:12117): u32 minor, u32 clk_id, u32 freq, u16 attri, u16 root, u32 enable - 20 bytes. */
typedef struct {
    uint32_t clk_id, clk_freq_in_hz, enable;
    uint16_t clk_attri, clk_root;
} AprClkSet;
/* afe_set_lpass_clk_cfg for the Primary TDM bit clock: sent with token AFE_CLK_TOKEN, ports 0 (q6afe.c:1983-2025, :2117-2148). */
uint32_t apr_afe_clock_set(uint8_t *out, uint32_t capacity, const AprClkSet *clk);

/* AFE port-level set-param. q6afe_set_params_v2 puts port_id and a u16 payload_size before the memory header; the v3 struct
 * carries a u16 reserved there and a u32 payload_size after it (q6afe.c:1587-1675). */
uint32_t apr_afe_port_set_param(uint8_t *out, uint32_t capacity, uint32_t token, uint16_t port_id, const AprParam *param);

/* struct afe_param_id_tdm_cfg (apr_audio-v2.h): four u32, eight u16, then the slot mask - 36 bytes. */
typedef struct {
    uint32_t num_channels, sample_rate, bit_width;
    uint16_t data_format, sync_mode, sync_src, nslots_per_frame;
    uint16_t ctrl_data_out_enable, ctrl_invert_sync_pulse, ctrl_sync_data_delay, slot_width;
    uint32_t slot_mask;
} AprTdmCfg;
uint32_t apr_afe_tdm_config(uint8_t *out, uint32_t capacity, uint32_t token, uint16_t port_id, const AprTdmCfg *cfg);

/* The slot mapping, in whichever of the two shapes the ADSP's AFE API version asks for: v1 carries eight offsets (28 bytes), v2
 * carries AFE_PORT_MAX_AUDIO_CHAN_V2 (76 bytes) with the unused ones set to AFE_SLOT_MAPPING_OFFSET_INVALID
 * (asoc/msm-dai-q6-v2.c:9376-9395, dsp/q6afe.c:4484-4542). */
typedef struct {
    uint16_t num_channels, bit_width;
    uint32_t data_align_type;                        /* 0 = MSB aligned */
    int version_2;
    const uint16_t *offsets;                         /* num_channels entries */
} AprSlotMap;
uint32_t apr_afe_slot_mapping(uint8_t *out, uint32_t capacity, uint32_t token, uint16_t port_id, const AprSlotMap *map);

/* struct afe_port_cmd_device_start / _stop: u16 port_id and u16 reserved, so a 24-byte packet (q6afe.c:1700-1780). */
uint32_t apr_afe_port_start(uint8_t *out, uint32_t capacity, uint32_t token, uint16_t port_id);
uint32_t apr_afe_port_stop(uint8_t *out, uint32_t capacity, uint32_t token, uint16_t port_id);
/* AFE_CMD_REMOTE_LPASS_CORE_HW_VOTE_REQUEST / _DROP: token = hw_block_id, name 8 bytes not NUL terminated. Not needed for TDM. */
uint32_t apr_afe_hw_vote(uint8_t *out, uint32_t capacity, uint32_t hw_block_id, const char client_name[8], int drop);

/* ADM_CMD_DEVICE_OPEN_V8 with one endpoint payload: endpoint 1 is the AFE port, 2 and 3 absent, PCM (q6adm.c:3355-3405). */
typedef struct {
    uint16_t port_id, mode_of_operation, num_channels, bit_width;
    uint32_t topology_id, sample_rate;
} AprAdmOpen;
uint32_t apr_adm_device_open(uint8_t *out, uint32_t capacity, uint32_t token, const AprAdmOpen *o);
/* ADM_CMDRSP_DEVICE_OPEN_V8 answers with struct adm_cmd_rsp_device_open_v5 {u32 status; u16 copp_id; u16 reserved} and the driver
 * stores that copp_id (q6adm.c:1836-1860); INVALID_COPP_ID 0xFF (q6adm.c:27) means the open failed. The matrix map then routes the
 * ASM session to the copp_id, never to the port id. */
#define ADM_INVALID_COPP_ID 0xFFu
int apr_adm_device_open_rsp(const AprMessage *m, uint32_t *status, uint16_t *copp_id);
/* ADM_CMD_MATRIX_MAP_ROUTINGS_V5: one session, the COPPs it routes to (q6adm.c:3717). */
uint32_t apr_adm_matrix_map(uint8_t *out, uint32_t capacity, uint32_t token, uint16_t session, const uint16_t *copps,
                            uint32_t num_copps);

/* ASM_CMD_SHARED_MEM_MAP_REGIONS: physical, 4 KB aligned regions (q6asm.c:8221-8290). The handle arrives in the answer. */
uint32_t apr_asm_memory_map(uint8_t *out, uint32_t capacity, uint32_t token, const uint64_t *addresses, const uint32_t *sizes,
                            uint32_t num_regions);
/* ASM_STREAM_CMD_OPEN_WRITE_V3: legacy session, matrix sink, multi-channel PCM v5 as the decoder id (q6asm.c:3460-3540). */
uint32_t apr_asm_open_write(uint8_t *out, uint32_t capacity, uint32_t token, uint8_t session, uint8_t stream_id,
                            uint16_t bits_per_sample, uint32_t postprocopo_id);
/* ASM_DATA_CMD_MEDIA_FMT_UPDATE_V2 in the v5 form this board uses (q6asm.c:7016-7060, called from msm-pcm-q6-v2.c:445-451):
 * signed, little endian, DEFAULT_QF mode and the default channel map. */
typedef struct {
    uint16_t num_channels, bits_per_sample, sample_word_size, mode;
    uint32_t sample_rate;
} AprPcmFormat;
uint32_t apr_asm_media_format(uint8_t *out, uint32_t capacity, uint32_t token, uint8_t session, uint8_t stream_id,
                              const AprPcmFormat *f);
/* ASM_SESSION_CMD_RUN_V2: flags 0 runs immediately. */
uint32_t apr_asm_run(uint8_t *out, uint32_t capacity, uint32_t token, uint8_t session, uint8_t stream_id, uint32_t flags,
                     uint64_t timestamp);
/* ASM_STREAM_CMD_SET_PP_PARAMS_V2/_V3 in band with one master gain: __q6asm_set_volume's packet (dsp/q6asm.c:8803), sent on the
 * session's own ports like every other ASM command. master_gain_q13 is the linear Q13 value, 16384 being unity. */
uint32_t apr_asm_set_volume(uint8_t *out, uint32_t capacity, uint32_t token, uint8_t session, uint8_t stream_id,
                            uint16_t master_gain_q13);
/* ASM_DATA_CMD_WRITE_V2: one mapped buffer (q6asm.c:9680-9702). flags bit 31 = timestamp valid, bit 4 = end of frame. */
uint32_t apr_asm_write(uint8_t *out, uint32_t capacity, uint32_t token, uint8_t session, uint8_t stream_id, uint64_t buffer,
                       uint32_t mem_map_handle, uint32_t length, uint32_t sequence, uint64_t timestamp, uint32_t flags);
uint32_t apr_asm_stream_close(uint8_t *out, uint32_t capacity, uint32_t token, uint8_t session, uint8_t stream_id);

/* ASM_CMD_SHARED_MEM_MAP_REGIONS is answered on the map channel with the new handle as the first u32 of the payload; the driver
 * waits for ac->port[dir].tmp_hdl to become non-zero and only then accepts it (q6asm.c:1809, :8268-8308). */
int apr_asm_mem_map_rsp(const AprMessage *m, uint32_t *handle);
/* ASM_DATA_EVENT_WRITE_DONE_V2: the completed buffer comes from the token's buf_index field, not the payload; the payload only
 * carries {opcode echo, status} and the driver rejects a shorter one (q6asm.c:2184-2214). */
int apr_asm_write_done(const AprMessage *m, uint8_t *buf_index, uint32_t *status);

/* The vendor's default channel map for n channels (q6adm.c:2751-2820, the same table q6asm uses for a format block). */
int apr_pcm_channel_map(uint8_t *map, uint32_t capacity, uint16_t channels);

/* --- pending requests --- */
#define APR_PENDING_SLOTS 8u
enum {AprPFree = 0, AprPWaiting = 1};
enum {AprPUnknown = 0, AprPAck = 1, AprPResult = 2, AprPEvent = 3};
typedef struct {
    uint32_t request, response, token;
    uint8_t state;
} AprPendingSlot;
typedef struct {
    uint32_t next_token, sent, answers, errors, timeouts, unmatched;
    uint32_t last_status, last_opcode;
    AprPendingSlot slot[APR_PENDING_SLOTS];
} AprPending;

void apr_pending_init(AprPending *p);
/* Reserves a slot for a request opcode and the answer opcode the vendor waits for (APR_OP_BASIC_RESULT when there is none) and
 * returns the token to put in the packet, 0 when every slot is busy. */
uint32_t apr_pending_add(AprPending *p, uint32_t request, uint32_t response);
/* An answer for a waiting request: frees the slot and returns AprPAck (the answer the ladder waits for), AprPEvent (a packet that
 * belongs to the request but is not its answer, e.g. a write-done), AprPResult (a basic result, status in *status) or
 * AprPUnknown. */
int apr_pending_reply(AprPending *p, const AprMessage *m, uint32_t *request_opcode, uint32_t *status);
/* The owner's timer for this token ran out; returns 1 when the slot was waiting. */
int apr_pending_timeout(AprPending *p, uint32_t token);
/* How many requests are still waiting for an answer. */
uint32_t apr_pending_waiting(const AprPending *p);

#ifdef __cplusplus
}
#endif
#endif
