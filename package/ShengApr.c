/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "ShengApr.h"

static void put16(uint8_t *p, uint16_t v) {p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);}
static void put32(uint8_t *p, uint32_t v) {p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);}
static uint16_t le16(const uint8_t *p) {return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);}
static uint32_t le32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}

static int iid_supported;

void apr_set_iid_supported(int supported) {iid_supported = supported ? 1 : 0;}
int apr_iid_supported(void) {return iid_supported;}

/* A bounded writer into the payload area; every builder fills the header last, once the size is known. */
typedef struct {uint8_t *data; uint32_t capacity, used;} Writer;
static void w_init(Writer *w, uint8_t *data, uint32_t capacity) {w->data = data; w->capacity = capacity; w->used = 0;}
static int w16(Writer *w, uint32_t v)
{
    if (w->used + 2u > w->capacity) return -1;
    put16(w->data + w->used, (uint16_t)v);
    w->used += 2u;
    return 0;
}
static int w32(Writer *w, uint32_t v)
{
    if (w->used + 4u > w->capacity) return -1;
    put32(w->data + w->used, v);
    w->used += 4u;
    return 0;
}
static int w_bytes(Writer *w, const void *bytes, uint32_t count)
{
    if (count > w->capacity - w->used) return -1;
    const uint8_t *src = (const uint8_t *)bytes;
    for (uint32_t i = 0; i < count; ++i) w->data[w->used + i] = src[i];
    w->used += count;
    return 0;
}
static int w_zero(Writer *w, uint32_t count)
{
    if (count > w->capacity - w->used) return -1;
    for (uint32_t i = 0; i < count; ++i) w->data[w->used + i] = 0;
    w->used += count;
    return 0;
}

uint32_t apr_encode(uint8_t *out, uint32_t capacity, uint8_t service, uint16_t port, uint32_t token, uint32_t opcode,
                    const void *payload, uint32_t payload_bytes)
{
    if (payload_bytes > APR_MAX_PACKET - APR_HEADER_BYTES || (payload_bytes && !payload)) return 0;
    const uint32_t size = APR_HEADER_BYTES + payload_bytes;
    if (size > capacity) return 0;
    /* Every one of these packets is a sequential command: the vendor drivers use APR_MSG_TYPE_SEQ_CMD for all of them. */
    put16(out, APR_HDR_FIELD(AprMsgSeqCmd, APR_HEADER_BYTES, APR_PKT_VER));
    put16(out + 2, (uint16_t)size);
    out[4] = service; out[5] = AprDomainAPPS;
    put16(out + 6, port);
    out[8] = service; out[9] = AprDomainADSP;
    put16(out + 10, port);
    put32(out + 12, token);
    put32(out + 16, opcode);
    const uint8_t *src = (const uint8_t *)payload;
    for (uint32_t i = 0; i < payload_bytes; ++i) out[APR_HEADER_BYTES + i] = src[i];
    return size;
}

/* A request whose payload is already written past the header: finish the header and check the transport's limit. */
static uint32_t finish(uint8_t *out, const Writer *w, uint8_t service, uint16_t port, uint32_t token, uint32_t opcode)
{
    return apr_encode(out, APR_HEADER_BYTES + w->capacity, service, port, token, opcode, out + APR_HEADER_BYTES, w->used);
}

int apr_decode(const uint8_t *in, uint32_t length, AprMessage *m)
{
    if (length < APR_HEADER_BYTES) return AprShort;
    const uint16_t field = le16(in);
    m->msg_type = (uint8_t)(field >> 8);
    m->header_bytes = ((field >> 4) & 0xfu) * 4u;
    m->size = le16(in + 2);
    if ((field & 0xfu) != APR_PKT_VER) return AprBadVersion;
    /* apr.c:634-647 rejects a packet smaller than the fixed header and smaller than its own header. */
    if (m->header_bytes < APR_HEADER_BYTES || m->size < APR_HEADER_BYTES || m->size < m->header_bytes) return AprBadHeader;
    if (m->size > length) return AprBadSize;         /* never read past what the channel actually delivered */
    m->src_svc = in[4]; m->src_domain = in[5]; m->src_port = le16(in + 6);
    m->dest_svc = in[8]; m->dest_domain = in[9]; m->dest_port = le16(in + 10);
    m->token = le32(in + 12);
    m->opcode = le32(in + 16);
    m->payload = in + m->header_bytes;
    m->payload_bytes = m->size - m->header_bytes;
    return AprOk;
}

int apr_basic_result(const AprMessage *m, uint32_t *opcode, uint32_t *status)
{
    if (m->opcode != APR_OP_BASIC_RESULT || m->payload_bytes < 8u) return -1;
    *opcode = le32(m->payload);
    *status = le32(m->payload + 4u);
    return 0;
}

uint32_t apr_core_get_state(uint8_t *out, uint32_t capacity, uint32_t token)
{
    return apr_encode(out, capacity, AprSvcAdspCore, 0, token, APR_OP_CORE_GET_STATE, 0, 0);
}

uint32_t apr_avcs_get_fwk_version(uint8_t *out, uint32_t capacity)
{
    return apr_encode(out, capacity, AprSvcAdspCore, 0, 0, APR_OP_AVCSS_GET_FWK_VERSION, 0, 0);
}

/* q6core caches the answer as struct avcs_fwk_ver_info: five u32 (build major/minor/branch/subbranch, num_services) then
 * num_services records of {service_id, api_version, api_branch_version}. */
int apr_avcs_service_api(const AprMessage *m, uint32_t service_id, uint32_t *api_version, uint32_t *num_services)
{
    if (m->opcode != APR_OP_AVCSS_GET_FWK_VERSION_RSP || m->payload_bytes < 20u) return -1;
    const uint32_t count = le32(m->payload + 16u);
    if (num_services) *num_services = count;
    if (count > APR_AVCS_SERVICE_API_MAX || m->payload_bytes < 20u + count * 12u) return -1;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *rec = m->payload + 20u + i * 12u;
        if (le32(rec) != service_id) continue;
        if (api_version) *api_version = le32(rec + 4u);
        return 0;
    }
    return -1;
}

/* struct param_hdr_v1 / _v3 plus the parameter, exactly as q6common_pack_pp_params_v2 lays them out; *packed is the size it
 * reports back to the command struct. */
static int pack_param(Writer *w, const AprParam *p, uint32_t *packed)
{
    if (!p || p->data_bytes > APR_MAX_PACKET || (p->data_bytes && !p->data)) return -1;
    const uint32_t header_bytes = iid_supported ? APR_PARAM_HDR_V3_BYTES : APR_PARAM_HDR_V1_BYTES;
    if (!iid_supported && p->data_bytes > 0xffffu) return -1;     /* the v1 header carries a u16 size */
    if (iid_supported) {
        if (w32(w, p->module_id) || w16(w, p->instance_id) || w16(w, 0)) return -1;
        if (w32(w, p->param_id) || w32(w, p->data_bytes)) return -1;
    } else {
        if (w32(w, p->module_id) || w32(w, p->param_id)) return -1;
        if (w16(w, p->data_bytes) || w16(w, 0)) return -1;
    }
    if (p->data_bytes && w_bytes(w, p->data, p->data_bytes)) return -1;
    *packed = header_bytes + p->data_bytes;
    return 0;
}

uint32_t apr_afe_svc_set_param(uint8_t *out, uint32_t capacity, uint32_t token, const AprParam *param)
{
    if (!param || !capacity) return 0;
    Writer w;
    w_init(&w, out + APR_HEADER_BYTES, capacity - APR_HEADER_BYTES);
    const uint32_t packed = (iid_supported ? APR_PARAM_HDR_V3_BYTES : APR_PARAM_HDR_V1_BYTES) + param->data_bytes;
    /* afe_svc_cmd_set_param_v1 has payload_size first; the v2 struct has it after the memory header. */
    if (!iid_supported) {if (w32(&w, packed) || w_zero(&w, APR_MEM_HDR_BYTES)) return 0;}
    else {if (w_zero(&w, APR_MEM_HDR_BYTES) || w32(&w, packed)) return 0;}
    uint32_t ignored = 0;
    if (pack_param(&w, param, &ignored)) return 0;
    return finish(out, &w, AprSvcAfe, 0, token,
                  iid_supported ? APR_OP_AFE_SVC_SET_PARAM_V2 : APR_OP_AFE_SVC_SET_PARAM);
}

uint32_t apr_afe_clock_set(uint8_t *out, uint32_t capacity, const AprClkSet *clk)
{
    if (!clk) return 0;
    uint8_t param[20];
    put32(param, AFE_CLK_API_VERSION);
    put32(param + 4, clk->clk_id);
    put32(param + 8, clk->clk_freq_in_hz);
    put16(param + 12, clk->clk_attri);
    put16(param + 14, clk->clk_root);
    put32(param + 16, clk->enable);
    const AprParam p = {AFE_MODULE_CLOCK_SET, AFE_PARAM_ID_CLOCK_SET, 0, param, sizeof param};
    return apr_afe_svc_set_param(out, capacity, AFE_CLK_TOKEN, &p);
}

uint32_t apr_afe_port_set_param(uint8_t *out, uint32_t capacity, uint32_t token, uint16_t port_id, const AprParam *param)
{
    if (!param || capacity < APR_HEADER_BYTES) return 0;
    Writer w;
    w_init(&w, out + APR_HEADER_BYTES, capacity - APR_HEADER_BYTES);
    const uint32_t packed = (iid_supported ? APR_PARAM_HDR_V3_BYTES : APR_PARAM_HDR_V1_BYTES) + param->data_bytes;
    if (!iid_supported) {
        if (packed > 0xffffu) return 0;
        if (w16(&w, port_id) || w16(&w, packed) || w_zero(&w, APR_MEM_HDR_BYTES)) return 0;
    } else {
        if (w16(&w, port_id) || w16(&w, 0) || w_zero(&w, APR_MEM_HDR_BYTES) || w32(&w, packed)) return 0;
    }
    uint32_t ignored = 0;
    if (pack_param(&w, param, &ignored)) return 0;
    return finish(out, &w, AprSvcAfe, 0, token,
                  iid_supported ? APR_OP_AFE_PORT_SET_PARAM_V3 : APR_OP_AFE_PORT_SET_PARAM_V2);
}

uint32_t apr_afe_tdm_config(uint8_t *out, uint32_t capacity, uint32_t token, uint16_t port_id, const AprTdmCfg *cfg)
{
    if (!cfg) return 0;
    uint8_t param[36];
    put32(param, AFE_API_VERSION_TDM_CONFIG);
    put32(param + 4, cfg->num_channels);
    put32(param + 8, cfg->sample_rate);
    put32(param + 12, cfg->bit_width);
    put16(param + 16, cfg->data_format);
    put16(param + 18, cfg->sync_mode);
    put16(param + 20, cfg->sync_src);
    put16(param + 22, cfg->nslots_per_frame);
    put16(param + 24, cfg->ctrl_data_out_enable);
    put16(param + 26, cfg->ctrl_invert_sync_pulse);
    put16(param + 28, cfg->ctrl_sync_data_delay);
    put16(param + 30, cfg->slot_width);
    put32(param + 32, cfg->slot_mask);
    const AprParam p = {AFE_MODULE_AUDIO_DEV_INTERFACE, AFE_PARAM_ID_TDM_CONFIG, 0, param, sizeof param};
    return apr_afe_port_set_param(out, capacity, token, port_id, &p);
}

uint32_t apr_afe_slot_mapping(uint8_t *out, uint32_t capacity, uint32_t token, uint16_t port_id, const AprSlotMap *map)
{
    if (!map || !map->offsets || !map->num_channels) return 0;
    if (!map->version_2 && map->num_channels > 8u) return 0;               /* v1 only carries eight offsets */
    if (map->num_channels > AFE_PORT_MAX_AUDIO_CHAN_V2) return 0;
    const uint32_t slots = map->version_2 ? AFE_PORT_MAX_AUDIO_CHAN_V2 : 8u;
    uint8_t param[4 + 2 + 2 + 4 + AFE_PORT_MAX_AUDIO_CHAN_V2 * 2];
    put32(param, map->version_2 ? AFE_API_VERSION_SLOT_MAPPING_V2 : AFE_API_VERSION_SLOT_MAPPING_V1);
    put16(param + 4, map->num_channels);
    put16(param + 6, map->bit_width);
    put32(param + 8, map->data_align_type);
    for (uint32_t i = 0; i < slots; ++i) {
        const uint16_t value = i < map->num_channels ? map->offsets[i] : (uint16_t)AFE_SLOT_MAPPING_OFFSET_INVALID;
        put16(param + 12 + i * 2u, value);
    }
    const AprParam p = {AFE_MODULE_TDM, AFE_PARAM_ID_PORT_SLOT_MAPPING_CONFIG, 0, param, 12u + slots * 2u};
    return apr_afe_port_set_param(out, capacity, token, port_id, &p);
}

/* struct afe_port_cmd_device_start: u16 port_id then u16 reserved, so the packet is 24 bytes. */
uint32_t apr_afe_port_start(uint8_t *out, uint32_t capacity, uint32_t token, uint16_t port_id)
{
    uint8_t payload[4];
    put16(payload, port_id);
    put16(payload + 2, 0);
    return apr_encode(out, capacity, AprSvcAfe, 0, token, APR_OP_AFE_PORT_START, payload, sizeof payload);
}

uint32_t apr_afe_port_stop(uint8_t *out, uint32_t capacity, uint32_t token, uint16_t port_id)
{
    uint8_t payload[4];
    put16(payload, port_id);
    put16(payload + 2, 0);
    return apr_encode(out, capacity, AprSvcAfe, 0, token, APR_OP_AFE_PORT_STOP, payload, sizeof payload);
}

uint32_t apr_afe_hw_vote(uint8_t *out, uint32_t capacity, uint32_t hw_block_id, const char client_name[8], int drop)
{
    if (!client_name) return 0;
    uint8_t payload[12];
    put32(payload, hw_block_id);
    for (uint32_t i = 0; i < 8u; ++i) payload[4 + i] = (uint8_t)client_name[i];
    return apr_encode(out, capacity, AprSvcAfe, 0, hw_block_id, drop ? APR_OP_AFE_HW_VOTE_DROP : APR_OP_AFE_HW_VOTE_REQUEST,
                      payload, sizeof payload);
}

uint32_t apr_adm_device_open(uint8_t *out, uint32_t capacity, uint32_t token, const AprAdmOpen *o)
{
    if (!o || !o->num_channels || o->num_channels > PCM_MAX_CHANNELS) return 0;
    uint8_t map[PCM_MAX_CHANNELS];
    if (apr_pcm_channel_map(map, sizeof map, o->num_channels)) return 0;
    const uint32_t mapping_bytes = ((uint32_t)o->num_channels + 3u) & ~3u;   /* roundup(dev_num_channel, 4) (q6adm.c:3398) */
    Writer w;
    w_init(&w, out + APR_HEADER_BYTES, capacity - APR_HEADER_BYTES);
    if (w16(&w, 0) || w16(&w, o->mode_of_operation)) return 0;              /* flags: legacy device session */
    if (w32(&w, o->topology_id)) return 0;
    if (w16(&w, o->port_id) || w16(&w, ADM_ENDPOINT_ABSENT) || w16(&w, ADM_ENDPOINT_ABSENT)) return 0;
    if (w16(&w, 0)) return 0;                                               /* compressed_data_type: PCM */
    if (w16(&w, o->num_channels) || w16(&w, o->bit_width) || w32(&w, o->sample_rate)) return 0;
    if (w_bytes(&w, map, mapping_bytes)) return 0;
    return finish(out, &w, AprSvcAdm, o->port_id, token, APR_OP_ADM_DEVICE_OPEN_V8);
}

/* struct adm_cmd_rsp_device_open_v5, the shape q6adm.c casts the V8 answer to: {u32 status, u16 copp_id, u16 reserved}
 * (q6adm.c:1836-1860, apr_audio-v2.h:724-733). */
int apr_adm_device_open_rsp(const AprMessage *m, uint32_t *status, uint16_t *copp_id)
{
    if (m->opcode != APR_OP_ADM_DEVICE_OPEN_RSP_V8 || m->payload_bytes < 8u) return -1;
    if (status) *status = le32(m->payload);
    if (copp_id) *copp_id = le16(m->payload + 4u);
    return 0;
}

uint32_t apr_adm_matrix_map(uint8_t *out, uint32_t capacity, uint32_t token, uint16_t session, const uint16_t *copps,
                            uint32_t num_copps)
{
    if (!copps || !num_copps || num_copps > ADM_MAX_COPPS) return 0;
    Writer w;
    w_init(&w, out + APR_HEADER_BYTES, capacity - APR_HEADER_BYTES);
    if (w32(&w, ADM_MATRIX_ID_AUDIO_RX) || w32(&w, 1)) return 0;            /* one session in this command */
    if (w16(&w, session) || w16(&w, num_copps)) return 0;
    /* q6adm.c:3722 sizes the command with four bytes per COPP but writes the ids as u16 with a stride of two, so the packet really
     * carries two zero bytes after the list; the ADSP of this board has only ever seen that shape. */
    for (uint32_t i = 0; i < num_copps; ++i) if (w16(&w, copps[i])) return 0;
    if (w_zero(&w, 2u * num_copps)) return 0;
    return finish(out, &w, AprSvcAdm, 0, token, APR_OP_ADM_MATRIX_MAP_V5);
}

/* The token an ASM packet carries (q6asm.c:61-69, built by q6asm_update_token at :161-174). The vendor always passes the session
 * id, and stream_id 0 for a command whose ports already name the stream; the buffer index and the two flag bits only matter for
 * the write path, where they identify the buffer a write-done event completes. */
uint32_t apr_asm_token(uint8_t session, uint8_t stream_id, uint8_t buf_index, uint32_t flags)
{
    uint32_t token = (uint32_t)stream_id | (uint32_t)session << 8 | (uint32_t)buf_index << 16;
    if (flags & ASM_TOKEN_DIR_OUT) token |= 1u << 24;
    if (flags & ASM_TOKEN_NO_WAIT) token |= 1u << 25;
    return token;
}
uint8_t apr_asm_token_buf_index(uint32_t token) {return (uint8_t)(token >> 16);}
uint8_t apr_asm_token_stream(uint32_t token) {return (uint8_t)token;}

uint32_t apr_asm_memory_map(uint8_t *out, uint32_t capacity, uint32_t token, const uint64_t *addresses, const uint32_t *sizes,
                            uint32_t num_regions)
{
    /* q6asm_memory_map_regions masks the count into the u16 field (num_regions = bufcnt & 0x00ff, q6asm.c:8239), so a count above
     * 255 would silently describe a different number of regions than the ones written. */
    if (!addresses || !sizes || !num_regions || num_regions > 0xffu) return 0;
    Writer w;
    w_init(&w, out + APR_HEADER_BYTES, capacity - APR_HEADER_BYTES);
    if (w16(&w, ADSP_MEMORY_MAP_SHMEM8_4K_POOL) || w16(&w, num_regions & 0xffu) || w32(&w, 0)) return 0;
    for (uint32_t i = 0; i < num_regions; ++i)
        if (w32(&w, (uint32_t)addresses[i]) || w32(&w, (uint32_t)(addresses[i] >> 32)) || w32(&w, sizes[i])) return 0;
    return finish(out, &w, AprSvcAsm, 0, token, APR_OP_ASM_MEM_MAP);
}

uint32_t apr_asm_open_write(uint8_t *out, uint32_t capacity, uint32_t token, uint8_t session, uint8_t stream_id,
                            uint16_t bits_per_sample, uint32_t postprocopo_id)
{
    Writer w;
    w_init(&w, out + APR_HEADER_BYTES, capacity - APR_HEADER_BYTES);
    /* struct asm_stream_cmd_open_write_v3 (apr_audio-v2.h:7848-7899): mode_flags, sink_endpointype, bits_per_sample,
     * postprocopo_id, dec_fmt_id. __q6asm_open_write starts mode_flags at 0x00 and ORs ASM_LEGACY_STREAM_SESSION, which is 0, so
     * the field stays zero for a legacy PCM session; gapless would set bit 31. */
    if (w32(&w, ASM_LEGACY_STREAM_SESSION) || w16(&w, ASM_END_POINT_DEVICE_MATRIX) || w16(&w, bits_per_sample)) return 0;
    if (w32(&w, postprocopo_id) || w32(&w, ASM_MEDIA_FMT_MULTI_CHANNEL_PCM_V5)) return 0;
    return finish(out, &w, AprSvcAsm, (uint16_t)((uint16_t)session << 8 | stream_id), token, APR_OP_ASM_OPEN_WRITE_V3);
}

uint32_t apr_asm_media_format(uint8_t *out, uint32_t capacity, uint32_t token, uint8_t session, uint8_t stream_id,
                              const AprPcmFormat *f)
{
    if (!f || !f->num_channels || f->num_channels > PCM_MAX_CHANNELS) return 0;
    uint8_t map[PCM_MAX_CHANNELS];
    if (apr_pcm_channel_map(map, sizeof map, f->num_channels)) return 0;
    Writer w;
    w_init(&w, out + APR_HEADER_BYTES, capacity - APR_HEADER_BYTES);
    if (w32(&w, ASM_FMT_BLOCK_BYTES)) return 0;                              /* fmt_blk_size */
    if (w16(&w, f->num_channels) || w16(&w, f->bits_per_sample) || w32(&w, f->sample_rate)) return 0;
    if (w16(&w, 1) || w16(&w, f->sample_word_size) || w16(&w, ASM_LITTLE_ENDIAN) || w16(&w, f->mode)) return 0;
    if (w_bytes(&w, map, PCM_MAX_CHANNELS)) return 0;
    return finish(out, &w, AprSvcAsm, (uint16_t)((uint16_t)session << 8 | stream_id), token, APR_OP_ASM_MEDIA_FMT_V2);
}

uint32_t apr_asm_run(uint8_t *out, uint32_t capacity, uint32_t token, uint8_t session, uint8_t stream_id, uint32_t flags,
                     uint64_t timestamp)
{
    Writer w;
    w_init(&w, out + APR_HEADER_BYTES, capacity - APR_HEADER_BYTES);
    if (w32(&w, flags) || w32(&w, (uint32_t)timestamp) || w32(&w, (uint32_t)(timestamp >> 32))) return 0;
    return finish(out, &w, AprSvcAsm, (uint16_t)((uint16_t)session << 8 | stream_id), token, APR_OP_ASM_RUN_V2);
}

/* struct asm_stream_cmd_set_pp_params (apr_audio-v2.h:8366-8382) with no out-of-band memory: q6asm_set_pp_params allocates the
 * whole struct, so the zeroed 12-byte memory header is on the wire even in band, payload_size follows it and the packed parameter
 * follows that (:2919-2947). q6asm_pack_and_set_pp_param_in_band produces the packed part through q6common_pack_pp_params, which
 * chooses the 12-byte v1 or 16-byte v3 parameter header by the same flag that chooses V2/V3 here (dsp/q6common.c:73-110). */
uint32_t apr_asm_set_volume(uint8_t *out, uint32_t capacity, uint32_t token, uint8_t session, uint8_t stream_id,
                            uint16_t master_gain_q13)
{
    if (!capacity) return 0;
    uint8_t gain[4];
    put16(gain, master_gain_q13);
    put16(gain + 2, 0);                                   /* "Clients must set this field to zero" (:9837-9838) */
    const AprParam p = {ASM_MODULE_ID_VOL_CTRL, ASM_PARAM_ID_VOL_CTRL_MASTER_GAIN, 0, gain, sizeof gain};
    Writer w;
    w_init(&w, out + APR_HEADER_BYTES, capacity - APR_HEADER_BYTES);
    const uint32_t packed = (iid_supported ? APR_PARAM_HDR_V3_BYTES : APR_PARAM_HDR_V1_BYTES) + p.data_bytes;
    if (w_zero(&w, APR_MEM_HDR_BYTES) || w32(&w, packed)) return 0;
    uint32_t ignored = 0;
    if (pack_param(&w, &p, &ignored)) return 0;
    return finish(out, &w, AprSvcAsm, (uint16_t)((uint16_t)session << 8 | stream_id), token,
                  iid_supported ? APR_OP_ASM_SET_PP_PARAMS_V3 : APR_OP_ASM_SET_PP_PARAMS_V2);
}

uint32_t apr_asm_write(uint8_t *out, uint32_t capacity, uint32_t token, uint8_t session, uint8_t stream_id, uint64_t buffer,
                       uint32_t mem_map_handle, uint32_t length, uint32_t sequence, uint64_t timestamp, uint32_t flags)
{
    Writer w;
    w_init(&w, out + APR_HEADER_BYTES, capacity - APR_HEADER_BYTES);
    if (w32(&w, (uint32_t)buffer) || w32(&w, (uint32_t)(buffer >> 32)) || w32(&w, mem_map_handle) || w32(&w, length)) return 0;
    if (w32(&w, sequence) || w32(&w, (uint32_t)timestamp) || w32(&w, (uint32_t)(timestamp >> 32)) || w32(&w, flags)) return 0;
    return finish(out, &w, AprSvcAsm, (uint16_t)((uint16_t)session << 8 | stream_id), token, APR_OP_ASM_WRITE_V2);
}

uint32_t apr_asm_stream_close(uint8_t *out, uint32_t capacity, uint32_t token, uint8_t session, uint8_t stream_id)
{
    return apr_encode(out, capacity, AprSvcAsm, (uint16_t)((uint16_t)session << 8 | stream_id), token, APR_OP_ASM_STREAM_CLOSE,
                      0, 0);
}

int apr_asm_mem_map_rsp(const AprMessage *m, uint32_t *handle)
{
    /* q6asm.c:1809 takes the handle straight from the first u32 of the payload and the caller only wakes when it is non-zero. */
    if (m->opcode != APR_OP_ASM_MEM_MAP_RSP || m->payload_bytes < 4u) return -1;
    *handle = le32(m->payload);
    return *handle ? 0 : -1;
}

int apr_asm_write_done(const AprMessage *m, uint8_t *buf_index, uint32_t *status)
{
    /* ASM_DATA_EVENT_WRITE_DONE_V2 carries {request opcode echo, status}; q6asm.c:2186 rejects a payload shorter than two u32 and
     * :2203 reads the completed buffer back out of the token instead of the payload. */
    if (m->opcode != APR_OP_ASM_WRITE_DONE_V2 || m->payload_bytes < 8u) return -1;
    *buf_index = apr_asm_token_buf_index(m->token);
    if (status) *status = le32(m->payload + 4u);
    return 0;
}

int apr_pcm_channel_map(uint8_t *map, uint32_t capacity, uint16_t channels)
{
    if (!map || capacity < PCM_MAX_CHANNELS || !channels || channels > PCM_MAX_CHANNELS) return -1;
    for (uint32_t i = 0; i < PCM_MAX_CHANNELS; ++i) map[i] = 0;
    /* adm_arrange_mch_map_v8 (q6adm.c:2751-2820), the mapping used when no custom map is installed. */
    switch (channels) {
        case 1: map[0] = PCM_CHANNEL_FC; break;
        case 2: map[0] = PCM_CHANNEL_FL; map[1] = PCM_CHANNEL_FR; break;
        case 3: map[0] = PCM_CHANNEL_FL; map[1] = PCM_CHANNEL_FR; map[2] = PCM_CHANNEL_FC; break;
        case 4: map[0] = PCM_CHANNEL_FL; map[1] = PCM_CHANNEL_FR; map[2] = PCM_CHANNEL_LS; map[3] = PCM_CHANNEL_RS; break;
        case 5: map[0] = PCM_CHANNEL_FL; map[1] = PCM_CHANNEL_FR; map[2] = PCM_CHANNEL_FC;
                map[3] = PCM_CHANNEL_LS; map[4] = PCM_CHANNEL_RS; break;
        case 6: map[0] = PCM_CHANNEL_FL; map[1] = PCM_CHANNEL_FR; map[2] = PCM_CHANNEL_LFE; map[3] = PCM_CHANNEL_FC;
                map[4] = PCM_CHANNEL_LS; map[5] = PCM_CHANNEL_RS; break;
        case 7: map[0] = PCM_CHANNEL_FL; map[1] = PCM_CHANNEL_FR; map[2] = PCM_CHANNEL_FC; map[3] = PCM_CHANNEL_LFE;
                map[4] = PCM_CHANNEL_LB; map[5] = PCM_CHANNEL_RB; map[6] = PCM_CHANNEL_CS; break;
        case 8: map[0] = PCM_CHANNEL_FL; map[1] = PCM_CHANNEL_FR; map[2] = PCM_CHANNEL_CS; map[3] = PCM_CHANNEL_LFE;
                map[4] = PCM_CHANNEL_LB; map[5] = PCM_CHANNEL_RB; map[6] = PCM_CHANNEL_LS; map[7] = PCM_CHANNEL_RS; break;
        default: return -1;                                                   /* beyond 8 the vendor needs a custom map */
    }
    return 0;
}

void apr_pending_init(AprPending *p)
{
    for (unsigned i = 0; i < sizeof *p; ++i) ((uint8_t *)p)[i] = 0;
    p->next_token = 1;
}

uint32_t apr_pending_add(AprPending *p, uint32_t request, uint32_t response)
{
    for (uint32_t i = 0; i < APR_PENDING_SLOTS; ++i) {
        if (p->slot[i].state != AprPFree) continue;
        p->slot[i].request = request;
        p->slot[i].response = response;
        p->slot[i].token = p->next_token++;
        p->slot[i].state = AprPWaiting;
        p->sent++;
        return p->slot[i].token;
    }
    return 0;
}

static void release(AprPending *p, uint32_t i) {p->slot[i].state = AprPFree;}

int apr_pending_reply(AprPending *p, const AprMessage *m, uint32_t *request_opcode, uint32_t *status)
{
    uint32_t rcode = 0, rstatus = 0;
    const int basic = !apr_basic_result(m, &rcode, &rstatus);
    uint32_t found = APR_PENDING_SLOTS;
    /* A basic result names the request it answers; anything else is matched by the token we sent. */
    for (uint32_t i = 0; i < APR_PENDING_SLOTS; ++i) {
        if (p->slot[i].state != AprPWaiting) continue;
        if (basic ? p->slot[i].request == rcode : p->slot[i].token == m->token) {found = i; break;}
    }
    if (found == APR_PENDING_SLOTS) {                    /* then the answer's own opcode, for the commands sent with token 0 */
        for (uint32_t i = 0; i < APR_PENDING_SLOTS; ++i) {
            if (p->slot[i].state == AprPWaiting && !basic && p->slot[i].response == m->opcode) {found = i; break;}
        }
    }
    if (found == APR_PENDING_SLOTS) {
        p->unmatched++;
        p->last_opcode = m->opcode;
        return AprPUnknown;
    }
    const uint32_t request = p->slot[found].request;
    const uint32_t response = p->slot[found].response;
    release(p, found);
    p->last_opcode = m->opcode;
    if (request_opcode) *request_opcode = request;
    if (basic) {
        p->errors++;
        p->last_status = rstatus;
        if (status) *status = rstatus;
        return AprPResult;
    }
    p->answers++;
    if (status) *status = 0;
    return m->opcode == response ? AprPAck : AprPEvent;
}

int apr_pending_timeout(AprPending *p, uint32_t token)
{
    for (uint32_t i = 0; i < APR_PENDING_SLOTS; ++i) {
        if (p->slot[i].state != AprPWaiting || p->slot[i].token != token) continue;
        release(p, i);
        p->timeouts++;
        return 1;
    }
    return 0;
}

uint32_t apr_pending_waiting(const AprPending *p)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < APR_PENDING_SLOTS; ++i) if (p->slot[i].state == AprPWaiting) ++n;
    return n;
}
