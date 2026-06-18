/* Spa A2DP LHDC V5 codec */
/* SPDX-FileCopyrightText: Copyright (C) 2026 */
/* SPDX-License-Identifier: MIT */

#include <arpa/inet.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <spa/param/audio/format.h>
#include <spa/param/props.h>
#include <spa/pod/parser.h>
#include <spa/utils/dict.h>
#include <spa/utils/string.h>

#include "lhdcv5-enc.h"
#include "lhdc_dec.h"
#include "rtp.h"
#include "media-codecs.h"

#define LHDCV5_VERSION			1
#define LHDCV5_FRAME_DURATION_MS	5
#define LHDCV5_ENC_FRAME_DURATION	50
#define LHDCV5_ENC_INTERVAL_MS		20
#define LHDCV5_MIN_MTU			300
#define LHDCV5_MAX_FRAME_BYTES		1536
#define LHDCV5_MPL_HDR_LEN		2
#define LHDCV5_HDR_NUM_SHIFT		2
#define LHDCV5_HDR_MAX_FRAMES		(UINT8_MAX >> LHDCV5_HDR_NUM_SHIFT)
#define LHDCV5_ABR_UP_RATE_TIME_CNT	3000
#define LHDCV5_ABR_DOWN_RATE_TIME_CNT	4
#define LHDCV5_ABR_UP_QUEUE_THRESHOLD	1
#define LHDCV5_ABR_DOWN_QUEUE_THRESHOLD	0
#define LHDCV5_ABR_DOWN_TARGET_STAGE	0

#define LHDCV5_QUALITY_LOW0		0
#define LHDCV5_QUALITY_LOW1		1
#define LHDCV5_QUALITY_LOW2		2
#define LHDCV5_QUALITY_LOW3		3
#define LHDCV5_QUALITY_LOW4		4
#define LHDCV5_QUALITY_LOW		5
#define LHDCV5_QUALITY_MID		6
#define LHDCV5_QUALITY_HIGH		7
#define LHDCV5_QUALITY_HIGH1		8
#define LHDCV5_QUALITY_HIGH2		9
#define LHDCV5_QUALITY_HIGH3		10
#define LHDCV5_QUALITY_HIGH4		11
#define LHDCV5_QUALITY_HIGH5		12
#define LHDCV5_QUALITY_AUTO		13

static struct spa_log *log_;

int g_nzc_formula = 0;
int g_pkframe = 0;

struct props {
	int quality;
};

struct impl {
	HANDLE_LHDC_BT enc;
	lhdc_decoder_t *dec;
	void *dec_workspace;

	struct rtp_header *header;
	uint8_t *payload;

	uint8_t *enc_tmp;
	size_t enc_tmp_size;

	size_t mtu;
	uint32_t rate;
	uint32_t frame_samples;
	uint32_t bits_per_sample;
	uint32_t pcm_sample_size;
	uint32_t pcm_format;
	uint32_t codesize;
	int quality;
	int current_quality;
	int min_quality;
	int max_quality;
	bool enable_abr;
	bool sink;

	const uint32_t *abr_table;
	uint32_t abr_table_size;
	uint32_t abr_table_index;
	uint32_t abr_down_count;
	uint32_t abr_down_sum;
	uint32_t abr_up_count;
	uint32_t abr_up_sum;

	uint8_t enc_frames;
	uint8_t packet_seq;
	uint32_t dec_frames;
};

static const uint32_t abr_table_44k[] = { 160, 192, 240, 320, 400, 400 };
static const uint32_t abr_table_48k[] = { 160, 192, 256, 320, 400, 400 };
static const uint32_t abr_table_96k[] = { 256, 320, 400, 400, 400, 400 };
static const uint32_t abr_table_192k[] = { 256, 320, 400, 400, 400, 400 };

static uint32_t lhdc_frame_samples(uint32_t rate)
{
	switch (rate) {
	case 44100:
	case 48000:
		return 240;
	case 96000:
		return 480;
	case 192000:
		return 960;
	default:
		return 0;
	}
}

static uint32_t lhdc_bits_from_caps(uint8_t bits)
{
	if (bits & LHDCV5_BITS_PER_SAMPLE_24)
		return 24;
	if (bits & LHDCV5_BITS_PER_SAMPLE_16)
		return 16;
	return 0;
}

static const char *quality_to_string(int quality)
{
	switch (quality) {
	case LHDCV5_QUALITY_AUTO:	return "auto";
	case LHDCV5_QUALITY_HIGH5:	return "high5";
	case LHDCV5_QUALITY_HIGH4:	return "high4";
	case LHDCV5_QUALITY_HIGH3:	return "high3";
	case LHDCV5_QUALITY_HIGH2:	return "high2";
	case LHDCV5_QUALITY_HIGH1:	return "high1";
	case LHDCV5_QUALITY_HIGH:	return "high";
	case LHDCV5_QUALITY_MID:	return "mid";
	case LHDCV5_QUALITY_LOW:	return "low";
	case LHDCV5_QUALITY_LOW4:	return "low4";
	case LHDCV5_QUALITY_LOW3:	return "low3";
	case LHDCV5_QUALITY_LOW2:	return "low2";
	case LHDCV5_QUALITY_LOW1:	return "low1";
	case LHDCV5_QUALITY_LOW0:	return "low0";
	default:			return "auto";
	}
}

static int string_to_quality(const char *quality)
{
	if (spa_streq(quality, "high5"))
		return LHDCV5_QUALITY_HIGH5;
	if (spa_streq(quality, "high4"))
		return LHDCV5_QUALITY_HIGH4;
	if (spa_streq(quality, "high3"))
		return LHDCV5_QUALITY_HIGH3;
	if (spa_streq(quality, "high2"))
		return LHDCV5_QUALITY_HIGH2;
	if (spa_streq(quality, "high1"))
		return LHDCV5_QUALITY_HIGH1;
	if (spa_streq(quality, "high"))
		return LHDCV5_QUALITY_HIGH;
	if (spa_streq(quality, "mid"))
		return LHDCV5_QUALITY_MID;
	if (spa_streq(quality, "low"))
		return LHDCV5_QUALITY_LOW;
	if (spa_streq(quality, "low4"))
		return LHDCV5_QUALITY_LOW4;
	if (spa_streq(quality, "low3"))
		return LHDCV5_QUALITY_LOW3;
	if (spa_streq(quality, "low2"))
		return LHDCV5_QUALITY_LOW2;
	if (spa_streq(quality, "low1"))
		return LHDCV5_QUALITY_LOW1;
	if (spa_streq(quality, "low0"))
		return LHDCV5_QUALITY_LOW0;
	return LHDCV5_QUALITY_AUTO;
}

static int codec_fill_caps(const struct media_codec *codec, uint32_t flags,
		const struct spa_dict *settings, uint8_t caps[A2DP_MAX_CAPS_SIZE])
{
	static const a2dp_lhdcv5_t a2dp_lhdc = {
		.info.vendor_id = LHDCV5_VENDOR_ID,
		.info.codec_id = LHDCV5_CODEC_ID,
		.frequency = LHDCV5_SAMPLING_FREQ_44100 |
			LHDCV5_SAMPLING_FREQ_48000 |
			LHDCV5_SAMPLING_FREQ_96000 |
			LHDCV5_SAMPLING_FREQ_192000,
		.bits_bitrate = LHDCV5_BITS_PER_SAMPLE_16 |
			LHDCV5_BITS_PER_SAMPLE_24 |
			LHDCV5_MAX_BIT_RATE_1000K |
			LHDCV5_MIN_BIT_RATE_64K,
		.version_frame = LHDCV5_VERSION_1 | LHDCV5_FRAME_LEN_5MS,
		.features = LHDCV5_FEATURE_LL,
		.features2 = 0,
	};

	memcpy(caps, &a2dp_lhdc, sizeof(a2dp_lhdc));
	return sizeof(a2dp_lhdc);
}

static const struct media_codec_config lhdc_frequencies[] = {
	{ LHDCV5_SAMPLING_FREQ_48000, 48000, 4 },
	{ LHDCV5_SAMPLING_FREQ_44100, 44100, 3 },
	{ LHDCV5_SAMPLING_FREQ_96000, 96000, 2 },
	{ LHDCV5_SAMPLING_FREQ_192000, 192000, 1 },
};

static const struct media_codec_config lhdc_bits[] = {
	{ LHDCV5_BITS_PER_SAMPLE_24, 24, 1 },
	{ LHDCV5_BITS_PER_SAMPLE_16, 16, 0 },
};

static int lhdc_max_quality_from_caps(uint8_t bits_bitrate)
{
	switch (bits_bitrate & LHDCV5_MAX_BIT_RATE_MASK) {
	case LHDCV5_MAX_BIT_RATE_1000K:
		return LHDCV5_QUALITY_HIGH1;
	case LHDCV5_MAX_BIT_RATE_900K:
		return LHDCV5_QUALITY_HIGH;
	case LHDCV5_MAX_BIT_RATE_500K:
		return LHDCV5_QUALITY_MID;
	case LHDCV5_MAX_BIT_RATE_400K:
		return LHDCV5_QUALITY_LOW;
	default:
		return LHDCV5_QUALITY_HIGH1;
	}
}

static int lhdc_min_quality_from_caps(uint8_t bits_bitrate)
{
	switch (bits_bitrate & LHDCV5_MIN_BIT_RATE_MASK) {
	case LHDCV5_MIN_BIT_RATE_400K:
		return LHDCV5_QUALITY_LOW;
	case LHDCV5_MIN_BIT_RATE_256K:
		return LHDCV5_QUALITY_LOW3;
	case LHDCV5_MIN_BIT_RATE_160K:
		return LHDCV5_QUALITY_LOW1;
	case LHDCV5_MIN_BIT_RATE_64K:
		return LHDCV5_QUALITY_LOW0;
	default:
		return LHDCV5_QUALITY_LOW0;
	}
}

static int codec_select_config(const struct media_codec *codec, uint32_t flags,
		const void *caps, size_t caps_size,
		const struct media_codec_audio_info *info,
		const struct spa_dict *settings, uint8_t config[A2DP_MAX_CAPS_SIZE],
		void **config_data)
{
	a2dp_lhdcv5_t conf;
	int i;

	if (caps_size < sizeof(conf))
		return -EINVAL;

	memcpy(&conf, caps, sizeof(conf));

	if (codec->vendor.vendor_id != conf.info.vendor_id ||
	    codec->vendor.codec_id != conf.info.codec_id)
		return -ENOTSUP;

	if ((conf.version_frame & LHDCV5_VERSION_MASK) == 0 ||
	    !(conf.version_frame & LHDCV5_VERSION_1) ||
	    !(conf.version_frame & LHDCV5_FRAME_LEN_5MS))
		return -ENOTSUP;

	if ((i = media_codec_select_config(lhdc_frequencies,
					  SPA_N_ELEMENTS(lhdc_frequencies),
					  conf.frequency,
					  info ? info->rate : A2DP_CODEC_DEFAULT_RATE)) < 0)
		return -ENOTSUP;
	conf.frequency = lhdc_frequencies[i].config;

	if (info && info->channels != 2)
		return -ENOTSUP;

	if ((i = media_codec_select_config(lhdc_bits,
					  SPA_N_ELEMENTS(lhdc_bits),
					  conf.bits_bitrate & LHDCV5_BITS_PER_SAMPLE_MASK,
					  24)) < 0)
		return -ENOTSUP;
	conf.bits_bitrate &= ~LHDCV5_BITS_PER_SAMPLE_MASK;
	conf.bits_bitrate |= lhdc_bits[i].config;

	conf.version_frame &= ~(LHDCV5_VERSION_MASK | LHDCV5_FRAME_LEN_MASK);
	conf.version_frame |= LHDCV5_VERSION_1 | LHDCV5_FRAME_LEN_5MS;
	conf.features &= LHDCV5_FEATURE_LL;
	conf.features2 = 0;

	memcpy(config, &conf, sizeof(conf));
	return sizeof(conf);
}

static int codec_enum_config(const struct media_codec *codec, uint32_t flags,
		const void *caps, size_t caps_size, uint32_t id, uint32_t idx,
		struct spa_pod_builder *b, struct spa_pod **param)
{
	a2dp_lhdcv5_t conf;
	struct spa_pod_frame f[2];
	struct spa_pod_choice *choice;
	uint32_t position[2] = { SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR };
	uint32_t i = 0;
	uint32_t bits;

	if (caps_size < sizeof(conf))
		return -EINVAL;

	memcpy(&conf, caps, sizeof(conf));

	if (idx > 0)
		return 0;

	bits = lhdc_bits_from_caps(conf.bits_bitrate);
	if (bits == 0)
		return -EINVAL;

	spa_pod_builder_push_object(b, &f[0], SPA_TYPE_OBJECT_Format, id);
	spa_pod_builder_add(b,
			SPA_FORMAT_mediaType,    SPA_POD_Id(SPA_MEDIA_TYPE_audio),
			SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
			0);

	if (bits == 16) {
		spa_pod_builder_add(b,
				SPA_FORMAT_AUDIO_format, SPA_POD_Id(SPA_AUDIO_FORMAT_S16),
				0);
	} else if (flags & MEDIA_CODEC_FLAG_SINK) {
		spa_pod_builder_add(b,
				SPA_FORMAT_AUDIO_format, SPA_POD_Id(SPA_AUDIO_FORMAT_S32),
				0);
	} else {
		spa_pod_builder_add(b,
				SPA_FORMAT_AUDIO_format, SPA_POD_CHOICE_ENUM_Id(3,
					SPA_AUDIO_FORMAT_S32,
					SPA_AUDIO_FORMAT_S32,
					SPA_AUDIO_FORMAT_S24),
				0);
	}

	spa_pod_builder_prop(b, SPA_FORMAT_AUDIO_rate, 0);
	spa_pod_builder_push_choice(b, &f[1], SPA_CHOICE_None, 0);
	choice = (struct spa_pod_choice *)spa_pod_builder_frame(b, &f[1]);
	i = 0;
	if (conf.frequency & LHDCV5_SAMPLING_FREQ_48000) {
		if (i++ == 0)
			spa_pod_builder_int(b, 48000);
		spa_pod_builder_int(b, 48000);
	}
	if (conf.frequency & LHDCV5_SAMPLING_FREQ_44100) {
		if (i++ == 0)
			spa_pod_builder_int(b, 44100);
		spa_pod_builder_int(b, 44100);
	}
	if (conf.frequency & LHDCV5_SAMPLING_FREQ_96000) {
		if (i++ == 0)
			spa_pod_builder_int(b, 96000);
		spa_pod_builder_int(b, 96000);
	}
	if (conf.frequency & LHDCV5_SAMPLING_FREQ_192000) {
		if (i++ == 0)
			spa_pod_builder_int(b, 192000);
		spa_pod_builder_int(b, 192000);
	}
	if (i > 1)
		choice->body.type = SPA_CHOICE_Enum;
	spa_pod_builder_pop(b, &f[1]);

	if (i == 0)
		return -EINVAL;

	spa_pod_builder_add(b,
			SPA_FORMAT_AUDIO_channels, SPA_POD_Int(2),
			SPA_FORMAT_AUDIO_position, SPA_POD_Array(sizeof(uint32_t),
				SPA_TYPE_Id, 2, position),
			0);

	*param = spa_pod_builder_pop(b, &f[0]);
	return *param == NULL ? -EIO : 1;
}

static void *codec_init_props(const struct media_codec *codec, uint32_t flags,
		const struct spa_dict *settings)
{
	struct props *p;
	const char *str;

	p = calloc(1, sizeof(*p));
	if (p == NULL)
		return NULL;

	if (settings == NULL || (str = spa_dict_lookup(settings, "bluez5.a2dp.lhdc.quality")) == NULL)
		str = "auto";

	p->quality = string_to_quality(str);
	return p;
}

static void codec_clear_props(void *props)
{
	free(props);
}

static int codec_enum_props(void *props, const struct spa_dict *settings, uint32_t id, uint32_t idx,
		struct spa_pod_builder *b, struct spa_pod **param)
{
	struct props *p = props;
	struct spa_pod_frame f[2];

	switch (id) {
	case SPA_PARAM_PropInfo:
		switch (idx) {
		case 0:
			spa_pod_builder_push_object(b, &f[0], SPA_TYPE_OBJECT_PropInfo, id);
			spa_pod_builder_prop(b, SPA_PROP_INFO_id, 0);
			spa_pod_builder_id(b, SPA_PROP_quality);
			spa_pod_builder_prop(b, SPA_PROP_INFO_description, 0);
			spa_pod_builder_string(b, "LHDC quality");

			spa_pod_builder_prop(b, SPA_PROP_INFO_type, 0);
			spa_pod_builder_push_choice(b, &f[1], SPA_CHOICE_Enum, 0);
			spa_pod_builder_int(b, p->quality);
			spa_pod_builder_int(b, LHDCV5_QUALITY_AUTO);
			spa_pod_builder_int(b, LHDCV5_QUALITY_HIGH5);
			spa_pod_builder_int(b, LHDCV5_QUALITY_HIGH4);
			spa_pod_builder_int(b, LHDCV5_QUALITY_HIGH3);
			spa_pod_builder_int(b, LHDCV5_QUALITY_HIGH2);
			spa_pod_builder_int(b, LHDCV5_QUALITY_HIGH1);
			spa_pod_builder_int(b, LHDCV5_QUALITY_HIGH);
			spa_pod_builder_int(b, LHDCV5_QUALITY_MID);
			spa_pod_builder_int(b, LHDCV5_QUALITY_LOW);
			spa_pod_builder_int(b, LHDCV5_QUALITY_LOW4);
			spa_pod_builder_int(b, LHDCV5_QUALITY_LOW3);
			spa_pod_builder_int(b, LHDCV5_QUALITY_LOW2);
			spa_pod_builder_int(b, LHDCV5_QUALITY_LOW1);
			spa_pod_builder_int(b, LHDCV5_QUALITY_LOW0);
			spa_pod_builder_pop(b, &f[1]);

			spa_pod_builder_prop(b, SPA_PROP_INFO_labels, 0);
			spa_pod_builder_push_struct(b, &f[1]);
			for (uint32_t q = LHDCV5_QUALITY_LOW0; q <= LHDCV5_QUALITY_AUTO; q++) {
				spa_pod_builder_int(b, q);
				spa_pod_builder_string(b, quality_to_string(q));
			}
			spa_pod_builder_pop(b, &f[1]);

			*param = spa_pod_builder_pop(b, &f[0]);
			break;
		default:
			return 0;
		}
		break;
	case SPA_PARAM_Props:
		switch (idx) {
		case 0:
			*param = spa_pod_builder_add_object(b,
				SPA_TYPE_OBJECT_Props, id,
				SPA_PROP_quality, SPA_POD_Int(p->quality));
			break;
		default:
			return 0;
		}
		break;
	default:
		return -ENOENT;
	}
	return 1;
}

static int codec_set_props(void *props, const struct spa_pod *param)
{
	struct props *p = props;
	int prev_quality = p->quality;

	if (param == NULL) {
		p->quality = LHDCV5_QUALITY_AUTO;
	} else {
		spa_pod_parse_object(param,
				SPA_TYPE_OBJECT_Props, NULL,
				SPA_PROP_quality, SPA_POD_OPT_Int(&p->quality));
		if (p->quality < LHDCV5_QUALITY_LOW0 || p->quality > LHDCV5_QUALITY_AUTO)
			p->quality = prev_quality;
	}
	return prev_quality != p->quality;
}

static int set_encoder_quality(struct impl *this, int quality, bool update_status)
{
	int target_quality = quality;

	if (this->enc == NULL)
		return -ENOTSUP;

	if (target_quality != LHDCV5_QUALITY_AUTO) {
		if (target_quality > this->max_quality)
			target_quality = this->max_quality;
		if (target_quality < this->min_quality)
			target_quality = this->min_quality;
	}

	if (lhdcv5_enc_ffi_set_bitrate_index(this->enc, (uint32_t)target_quality, update_status) != 0)
		return -EIO;

	this->current_quality = target_quality == LHDCV5_QUALITY_AUTO ? LHDCV5_QUALITY_LOW : target_quality;
	if (this->current_quality > this->max_quality)
		this->current_quality = this->max_quality;
	if (this->current_quality < this->min_quality)
		this->current_quality = this->min_quality;
	return 0;
}

static void reset_abr(struct impl *this)
{
	this->abr_table_index = this->abr_table_size > 0 ? this->abr_table_size - 1 : 0;
	this->abr_down_count = 0;
	this->abr_down_sum = 0;
	this->abr_up_count = 0;
	this->abr_up_sum = 0;
}

static void init_abr(struct impl *this)
{
	switch (this->rate) {
	case 44100:
		this->abr_table = abr_table_44k;
		this->abr_table_size = sizeof(abr_table_44k) / sizeof(abr_table_44k[0]);
		break;
	case 96000:
		this->abr_table = abr_table_96k;
		this->abr_table_size = sizeof(abr_table_96k) / sizeof(abr_table_96k[0]);
		break;
	case 192000:
		this->abr_table = abr_table_192k;
		this->abr_table_size = sizeof(abr_table_192k) / sizeof(abr_table_192k[0]);
		break;
	case 48000:
	default:
		this->abr_table = abr_table_48k;
		this->abr_table_size = sizeof(abr_table_48k) / sizeof(abr_table_48k[0]);
		break;
	}

	reset_abr(this);
}

static uint32_t queue_bytes_to_packets(struct impl *this, size_t unsent)
{
	size_t packet_size = sizeof(struct rtp_header) + LHDCV5_MPL_HDR_LEN + this->mtu;
	size_t packets;

	if (unsent == 0 || packet_size == 0)
		return 0;

	packets = (unsent + packet_size - 1) / packet_size;
	return packets > UINT32_MAX ? UINT32_MAX : (uint32_t)packets;
}

static int abr_set_bitrate(struct impl *this, uint32_t bitrate, bool reset_up, bool reset_down)
{
	uint32_t bitrate_index;

	if (lhdcv5_enc_ffi_get_bitrate_index(this->enc, bitrate, &bitrate_index) != 0)
		return -EIO;
	if (set_encoder_quality(this, (int)bitrate_index, false) < 0)
		return -EIO;

	if (reset_up) {
		this->abr_up_count = 0;
		this->abr_up_sum = 0;
	}
	if (reset_down) {
		this->abr_down_count = 0;
		this->abr_down_sum = 0;
	}

	return 0;
}

static void convert_s32_to_s24(const int32_t *src, uint8_t *dst, uint32_t samples)
{
	for (uint32_t i = 0; i < samples; i++) {
		int32_t s = src[i] >> 8;
		dst[3 * i + 0] = (uint8_t)(s & 0xff);
		dst[3 * i + 1] = (uint8_t)((s >> 8) & 0xff);
		dst[3 * i + 2] = (uint8_t)((s >> 16) & 0xff);
	}
}

static void *codec_init(const struct media_codec *codec, uint32_t flags,
		void *config, size_t config_len, const struct spa_audio_info *info,
		void *props, size_t mtu)
{
	struct impl *this = NULL;
	a2dp_lhdcv5_t conf;
	struct props *p = props;
	int res;

	if (config_len < sizeof(conf)) {
		res = -EINVAL;
		goto error;
	}
	if (info->media_type != SPA_MEDIA_TYPE_audio ||
	    info->media_subtype != SPA_MEDIA_SUBTYPE_raw ||
	    info->info.raw.channels != 2) {
		res = -EINVAL;
		goto error;
	}

	memcpy(&conf, config, sizeof(conf));

	if ((this = calloc(1, sizeof(*this))) == NULL)
		goto error_errno;

	this->sink = flags & MEDIA_CODEC_FLAG_SINK;
	this->rate = info->info.raw.rate;
	this->frame_samples = lhdc_frame_samples(this->rate);
	this->bits_per_sample = lhdc_bits_from_caps(conf.bits_bitrate);
	this->pcm_format = info->info.raw.format;
	this->max_quality = lhdc_max_quality_from_caps(conf.bits_bitrate);
	this->min_quality = lhdc_min_quality_from_caps(conf.bits_bitrate);
	this->quality = p ? p->quality : LHDCV5_QUALITY_AUTO;
	this->enable_abr = this->quality == LHDCV5_QUALITY_AUTO;
	this->current_quality = this->enable_abr ? LHDCV5_QUALITY_LOW : this->quality;
	init_abr(this);
	if (this->current_quality > this->max_quality)
		this->current_quality = this->max_quality;
	if (this->current_quality < this->min_quality)
		this->current_quality = this->min_quality;

	if (this->frame_samples == 0 || this->bits_per_sample == 0) {
		res = -EINVAL;
		goto error;
	}

	switch (this->pcm_format) {
	case SPA_AUDIO_FORMAT_S16:
		if (this->bits_per_sample != 16) {
			res = -EINVAL;
			goto error;
		}
		this->pcm_sample_size = 2;
		break;
	case SPA_AUDIO_FORMAT_S24:
		if (this->sink || this->bits_per_sample != 24) {
			res = -EINVAL;
			goto error;
		}
		this->pcm_sample_size = 3;
		break;
	case SPA_AUDIO_FORMAT_S32:
		if (this->bits_per_sample != 24) {
			res = -EINVAL;
			goto error;
		}
		this->pcm_sample_size = 4;
		break;
	default:
		res = -EINVAL;
		goto error;
	}

	this->codesize = this->frame_samples * 2 * this->pcm_sample_size;
	this->mtu = mtu > sizeof(struct rtp_header) + LHDCV5_MPL_HDR_LEN ?
		mtu - sizeof(struct rtp_header) - LHDCV5_MPL_HDR_LEN : mtu;

	if (this->sink) {
		lhdc_dec_config_t dec_conf = {
			.sample_rate = (lhdc_dec_sample_rate_t)this->rate,
			.bit_depth = this->bits_per_sample == 16 ?
				LHDC_DEC_BITDEPTH_16 : LHDC_DEC_BITDEPTH_24,
			.frame_duration = LHDC_DEC_FRAME_5MS,
			.channels = 2,
			.max_frame_bytes = LHDCV5_MAX_FRAME_BYTES,
			.lossless_enable = 0,
		};
		size_t workspace_size = lhdc_dec_get_workspace_size(this->rate, LHDCV5_FRAME_DURATION_MS);

		this->dec_workspace = calloc(1, workspace_size);
		if (this->dec_workspace == NULL)
			goto error_errno;
		this->dec = lhdc_dec_init(this->dec_workspace, &dec_conf);
		if (this->dec == NULL) {
			res = -EIO;
			goto error;
		}
	} else {
		uint32_t samples_per_frame;

		if (this->mtu < LHDCV5_MIN_MTU) {
			res = -EINVAL;
			goto error;
		}

		if (this->pcm_format == SPA_AUDIO_FORMAT_S32) {
			this->enc_tmp_size = this->frame_samples * 2 * 3;
			this->enc_tmp = malloc(this->enc_tmp_size);
			if (this->enc_tmp == NULL)
				goto error_errno;
		}

		lhdcv5_enc_ffi_init();
		if (lhdcv5_enc_ffi_get_handle(LHDCV5_VERSION, &this->enc) != 0) {
			res = -EIO;
			goto error;
		}
		if (lhdcv5_enc_ffi_init_encoder(this->enc, this->rate,
					this->bits_per_sample, this->quality,
					this->mtu, LHDCV5_ENC_INTERVAL_MS) != 0) {
			res = -EIO;
			goto error;
		}
		if (lhdcv5_enc_ffi_set_max_bitrate(this->enc, (uint32_t)this->max_quality) != 0 ||
		    lhdcv5_enc_ffi_set_min_bitrate(this->enc, (uint32_t)this->min_quality) != 0 ||
		    set_encoder_quality(this, this->quality, true) < 0) {
			res = -EIO;
			goto error;
		}
		if (lhdcv5_enc_ffi_get_block_size(this->enc, &samples_per_frame) != 0 ||
		    samples_per_frame != this->frame_samples) {
			res = -EIO;
			goto error;
		}
	}

	return this;

error_errno:
	res = -errno;
error:
	if (this) {
		if (this->enc)
			lhdcv5_enc_ffi_free_handle(this->enc);
		free(this->enc_tmp);
		free(this->dec_workspace);
		free(this);
	}
	errno = -res;
	return NULL;
}

static void codec_deinit(void *data)
{
	struct impl *this = data;

	if (this->enc)
		lhdcv5_enc_ffi_free_handle(this->enc);
	free(this->enc_tmp);
	free(this->dec_workspace);
	free(this);
}

static int codec_update_props(void *data, void *props)
{
	struct impl *this = data;
	struct props *p = props;

	if (p == NULL || this->sink)
		return 0;

	this->quality = p->quality;
	this->enable_abr = p->quality == LHDCV5_QUALITY_AUTO;
	if (this->enable_abr)
		reset_abr(this);
	return set_encoder_quality(this, p->quality, true);
}

static int codec_get_block_size(void *data)
{
	struct impl *this = data;
	return this->codesize;
}

static int codec_abr_process(void *data, size_t unsent)
{
	struct impl *this = data;
	uint32_t queue_len, last_bitrate, last_bitrate_index, new_bitrate_index;

	if (!this->enable_abr || this->sink || this->enc == NULL || this->abr_table_size == 0)
		return this->current_quality;

	queue_len = queue_bytes_to_packets(this, unsent);

	if (this->abr_down_count >= LHDCV5_ABR_DOWN_RATE_TIME_CNT) {
		uint32_t queue_avg = this->abr_down_count > 0 ?
			this->abr_down_sum / this->abr_down_count : 0;

		this->abr_down_count = 0;
		this->abr_down_sum = 0;

		if (queue_avg > LHDCV5_ABR_DOWN_QUEUE_THRESHOLD) {
			uint32_t new_stage = LHDCV5_ABR_DOWN_TARGET_STAGE;
			uint32_t new_bitrate = this->abr_table[new_stage];

			if (lhdcv5_enc_ffi_get_last_bitrate(this->enc, &last_bitrate) != 0 ||
			    lhdcv5_enc_ffi_get_bitrate_index(this->enc, last_bitrate, &last_bitrate_index) != 0 ||
			    lhdcv5_enc_ffi_get_bitrate_index(this->enc, new_bitrate, &new_bitrate_index) != 0)
				return -EIO;

			if (new_bitrate_index <= last_bitrate_index && new_stage < this->abr_table_index) {
				if (abr_set_bitrate(this, new_bitrate, true, false) < 0)
					return -EIO;
				this->abr_table_index = new_stage;
			}
		}
	}

	if (this->abr_up_count >= LHDCV5_ABR_UP_RATE_TIME_CNT) {
		uint32_t queue_sum = this->abr_up_sum;

		this->abr_up_count = 0;
		this->abr_up_sum = 0;

		if (queue_sum < LHDCV5_ABR_UP_QUEUE_THRESHOLD) {
			uint32_t new_stage = this->abr_table_index;
			uint32_t new_bitrate;

			if (new_stage < this->abr_table_size - 1)
				new_stage++;
			new_bitrate = this->abr_table[new_stage];

			if (lhdcv5_enc_ffi_get_last_bitrate(this->enc, &last_bitrate) != 0 ||
			    lhdcv5_enc_ffi_get_bitrate_index(this->enc, last_bitrate, &last_bitrate_index) != 0 ||
			    lhdcv5_enc_ffi_get_bitrate_index(this->enc, new_bitrate, &new_bitrate_index) != 0)
				return -EIO;

			if (new_bitrate_index >= last_bitrate_index && new_stage > this->abr_table_index) {
				if (abr_set_bitrate(this, new_bitrate, false, true) < 0)
					return -EIO;
				this->abr_table_index = new_stage;
			}
		}
	}

	this->abr_up_sum = UINT32_MAX - this->abr_up_sum < queue_len ?
		UINT32_MAX : this->abr_up_sum + queue_len;
	this->abr_down_sum = UINT32_MAX - this->abr_down_sum < queue_len ?
		UINT32_MAX : this->abr_down_sum + queue_len;
	if (this->abr_up_count < UINT32_MAX)
		this->abr_up_count++;
	if (this->abr_down_count < UINT32_MAX)
		this->abr_down_count++;

	return this->current_quality;
}

static int codec_reduce_bitpool(void *data)
{
	struct impl *this = data;

	if (!this->enable_abr)
		return this->current_quality;

	if (this->abr_table_size == 0 || this->abr_table_index == LHDCV5_ABR_DOWN_TARGET_STAGE)
		return this->current_quality;

	if (abr_set_bitrate(this, this->abr_table[LHDCV5_ABR_DOWN_TARGET_STAGE], true, true) < 0)
		return -EIO;
	this->abr_table_index = LHDCV5_ABR_DOWN_TARGET_STAGE;
	return this->current_quality;
}

static int codec_increase_bitpool(void *data)
{
	struct impl *this = data;

	return this->current_quality;
}

static int codec_start_encode(void *data,
		void *dst, size_t dst_size, uint16_t seqnum, uint32_t timestamp)
{
	struct impl *this = data;
	size_t header_size = sizeof(struct rtp_header) + LHDCV5_MPL_HDR_LEN;

	if (dst_size < header_size)
		return -EINVAL;

	this->header = dst;
	this->payload = SPA_PTROFF(dst, sizeof(struct rtp_header), uint8_t);
	memset(this->header, 0, header_size);

	this->enc_frames = 0;
	/* LHDC V5 carries shifted frame count in byte 0 and packet sequence in byte 1. */
	this->payload[1] = this->packet_seq++;
	this->header->v = 2;
	this->header->pt = 96;
	this->header->sequence_number = htons(seqnum);
	this->header->timestamp = htonl(timestamp);
	this->header->ssrc = htonl(1);
	return header_size;
}

static int codec_encode(void *data,
		const void *src, size_t src_size,
		void *dst, size_t dst_size,
		size_t *dst_out, int *need_flush)
{
	struct impl *this = data;
	const uint8_t *pcm = src;
	size_t pcm_size = src_size;
	uint32_t written = 0, frames = 0;

	if (src == NULL || src_size < this->codesize)
		return -EINVAL;

	if (this->pcm_format == SPA_AUDIO_FORMAT_S32) {
		convert_s32_to_s24(src, this->enc_tmp, this->frame_samples * 2);
		pcm = this->enc_tmp;
		pcm_size = this->enc_tmp_size;
	}

	if (lhdcv5_enc_ffi_encode(this->enc, pcm, pcm_size, dst, dst_size,
				&written, &frames) != 0)
		return -EINVAL;

	*dst_out = written;
	this->enc_frames += SPA_MIN(frames, LHDCV5_HDR_MAX_FRAMES - this->enc_frames);
	this->payload[0] = this->enc_frames << LHDCV5_HDR_NUM_SHIFT;
	*need_flush = frames > 0 ? NEED_FLUSH_ALL : NEED_FLUSH_NO;

	return this->codesize;
}

static int codec_start_decode(void *data,
		const void *src, size_t src_size, uint16_t *seqnum, uint32_t *timestamp)
{
	struct impl *this = data;
	const struct rtp_header *header = src;
	const uint8_t *payload = SPA_PTROFF(src, sizeof(struct rtp_header), uint8_t);
	size_t header_size = sizeof(struct rtp_header) + LHDCV5_MPL_HDR_LEN;

	if (src_size <= header_size)
		return -EINVAL;

	if (seqnum)
		*seqnum = ntohs(header->sequence_number);
	if (timestamp)
		*timestamp = ntohl(header->timestamp);

	this->dec_frames = payload[0] >> LHDCV5_HDR_NUM_SHIFT;
	return header_size;
}

static int codec_decode(void *data,
		const void *src, size_t src_size,
		void *dst, size_t dst_size,
		size_t *dst_out)
{
	struct impl *this = data;
	uint8_t *to = dst;
	size_t avail = dst_size;
	size_t processed = 0;
	uint32_t frame_samples = this->frame_samples;
	uint32_t out_frame_bytes = frame_samples * 2 * this->pcm_sample_size;

	*dst_out = 0;

	while (this->dec_frames > 0 && src_size > 0) {
		size_t consumed = 0;
		uint32_t generated = 0;
		lhdc_dec_frame_info_t frame_info;
		lhdc_dec_ret_t ret;

		if (avail < out_frame_bytes)
			return -EINVAL;

		ret = lhdc_dec_decode_frame(this->dec, src, src_size, to,
				frame_samples, &consumed, &generated, &frame_info);
		if (ret != LHDC_DEC_OK || consumed == 0 || consumed > src_size)
			return -EINVAL;
		if (generated > frame_samples)
			return -EINVAL;

		size_t produced = generated * 2 * this->pcm_sample_size;
		if (produced > avail)
			return -EINVAL;

		to = SPA_PTROFF(to, produced, uint8_t);
		avail -= produced;
		*dst_out += produced;

		src = SPA_PTROFF(src, consumed, const void);
		src_size -= consumed;
		processed += consumed;
		this->dec_frames--;
	}

	return processed;
}

static void codec_get_delay(void *data, uint32_t *encoder, uint32_t *decoder)
{
	struct impl *this = data;

	if (encoder)
		*encoder = this->frame_samples;
	if (decoder)
		*decoder = this->frame_samples;
}

static void codec_set_log(struct spa_log *global_log)
{
	log_ = global_log;
	spa_log_topic_init(log_, &codec_plugin_log_topic);
}

const struct media_codec a2dp_codec_lhdc = {
	.id = SPA_BLUETOOTH_AUDIO_CODEC_LHDC,
	.kind = MEDIA_CODEC_A2DP,
	.codec_id = A2DP_CODEC_VENDOR,
	.vendor = { .vendor_id = LHDCV5_VENDOR_ID,
		.codec_id = LHDCV5_CODEC_ID },
	.name = "lhdc",
	.description = "LHDC V5",
	.fill_caps = codec_fill_caps,
	.select_config = codec_select_config,
	.enum_config = codec_enum_config,
	.init_props = codec_init_props,
	.clear_props = codec_clear_props,
	.enum_props = codec_enum_props,
	.set_props = codec_set_props,
	.init = codec_init,
	.deinit = codec_deinit,
	.update_props = codec_update_props,
	.get_block_size = codec_get_block_size,
	.abr_process = codec_abr_process,
	.start_encode = codec_start_encode,
	.encode = codec_encode,
	.start_decode = codec_start_decode,
	.decode = codec_decode,
	.reduce_bitpool = codec_reduce_bitpool,
	.increase_bitpool = codec_increase_bitpool,
	.set_log = codec_set_log,
	.get_delay = codec_get_delay,
};

MEDIA_CODEC_EXPORT_DEF(
	"lhdc",
	&a2dp_codec_lhdc
);
