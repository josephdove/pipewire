/* LHDC V5 encoder C ABI */
/* SPDX-License-Identifier: Apache-2.0 */

#ifndef SPA_BLUEZ5_LHDCV5_ENC_H_
#define SPA_BLUEZ5_LHDCV5_ENC_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef const void *HANDLE_LHDC_BT;
typedef int32_t STATUS_LHDC_BT;

void lhdcv5_enc_ffi_init(void);
STATUS_LHDC_BT lhdcv5_enc_ffi_get_handle(uint32_t version, HANDLE_LHDC_BT *handle);
STATUS_LHDC_BT lhdcv5_enc_ffi_free_handle(HANDLE_LHDC_BT handle);
STATUS_LHDC_BT lhdcv5_enc_ffi_init_encoder(HANDLE_LHDC_BT handle,
		uint32_t sampling_freq, uint32_t bits_per_sample,
		uint32_t bitrate_inx, uint32_t mtu, uint32_t interval);
STATUS_LHDC_BT lhdcv5_enc_ffi_get_quality_mode(HANDLE_LHDC_BT handle,
		uint32_t *quality_status);
STATUS_LHDC_BT lhdcv5_enc_ffi_get_last_bitrate(HANDLE_LHDC_BT handle,
		uint32_t *bitrate);
STATUS_LHDC_BT lhdcv5_enc_ffi_get_bitrate_index(HANDLE_LHDC_BT handle,
		uint32_t bitrate, uint32_t *bitrate_inx);
STATUS_LHDC_BT lhdcv5_enc_ffi_set_bitrate_index(HANDLE_LHDC_BT handle,
		uint32_t bitrate_inx, bool upd_qual_status);
STATUS_LHDC_BT lhdcv5_enc_ffi_set_max_bitrate(HANDLE_LHDC_BT handle,
		uint32_t max_bitrate_inx);
STATUS_LHDC_BT lhdcv5_enc_ffi_set_min_bitrate(HANDLE_LHDC_BT handle,
		uint32_t min_bitrate_inx);
STATUS_LHDC_BT lhdcv5_enc_ffi_get_block_size(HANDLE_LHDC_BT handle,
		uint32_t *samples_per_frame);
STATUS_LHDC_BT lhdcv5_enc_ffi_encode(HANDLE_LHDC_BT handle,
		const uint8_t *in_pcm, size_t in_pcm_len,
		uint8_t *out_buf, size_t out_buf_len,
		uint32_t *written_bytes, uint32_t *written_frames);

#endif
