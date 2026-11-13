/* RFC 4867 mono RTP payloads, without CRC, interleaving or robust sorting.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef AST_AMR_PAYLOAD_H
#define AST_AMR_PAYLOAD_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define AMR_MAX_FRAMES 50
#define AMR_STORAGE_BYTES 61

struct amr_packet {
	unsigned int count;
	unsigned int cmr;
	struct {
		size_t offset;
		unsigned int bits;
		unsigned char toc;
	} frames[AMR_MAX_FRAMES];
};

static inline int amr_frame_bits(int wideband, unsigned int ft)
{
	static const int nb[16] = {95, 103, 118, 134, 148, 159, 204, 244, 39, -1, -1, -1, -1, -1, -1, 0};
	static const int wb[16] = {132, 177, 253, 285, 317, 365, 397, 461, 477, 40, -1, -1, -1, -1, 0, 0};
	return ft < 16 ? (wideband ? wb[ft] : nb[ft]) : -1;
}

/* Callers check the complete bit range before reading or writing. */
static inline unsigned int amr_bits_get(const unsigned char *src, size_t bit, unsigned int count)
{
	unsigned int value = 0;
	while (count--) {
		value = (value << 1) | ((src[bit / 8] >> (7 - bit % 8)) & 1);
		++bit;
	}
	return value;
}

static inline void amr_bits_copy(unsigned char *dst, size_t dst_bit,
	const unsigned char *src, size_t src_bit, unsigned int count)
{
	while (count--) {
		if (amr_bits_get(src, src_bit++, 1)) {
			dst[dst_bit / 8] |= 1U << (7 - dst_bit % 8);
		}
		++dst_bit;
	}
}

static inline int amr_payload_parse(const unsigned char *data, size_t len,
	int wideband, int aligned, struct amr_packet *packet)
{
	size_t bit = aligned ? 8 : 4;
	size_t end;
	unsigned int toc, i;
	int bits;

	/* Maximum mono packet: 50 WB frames with headers and padding. */
	if (!data || !packet || len < 2 || len > 1 + AMR_MAX_FRAMES * AMR_STORAGE_BYTES) {
		return -1;
	}
	end = len * 8;
	packet->count = 0;
	packet->cmr = data[0] >> 4;
	do {
		if (packet->count == AMR_MAX_FRAMES || bit + (aligned ? 8 : 6) > end) {
			return -1;
		}
		toc = amr_bits_get(data, bit, 6);
		bit += aligned ? 8 : 6;
		bits = amr_frame_bits(wideband, (toc >> 1) & 15);
		if (bits < 0) {
			return -1;
		}
		packet->frames[packet->count].bits = bits;
		packet->frames[packet->count++].toc = (toc & 31) << 2;
	} while (toc & 32);

	for (i = 0; i < packet->count; ++i) {
		packet->frames[i].offset = bit;
		bit += aligned ? ((packet->frames[i].bits + 7) / 8) * 8 : packet->frames[i].bits;
		if (bit > end) {
			return -1;
		}
	}
	/* Only the last octet may contain trailing padding. */
	return (bit + 7) / 8 == len ? 0 : -1;
}

/* Extract a validated frame into the MIME storage format OpenCORE expects. */
static inline int amr_payload_frame(const unsigned char *data,
	const struct amr_packet *packet, unsigned int index, unsigned char *out)
{
	unsigned int bits = packet->frames[index].bits;
	memset(out, 0, AMR_STORAGE_BYTES);
	out[0] = packet->frames[index].toc;
	amr_bits_copy(out, 8, data, packet->frames[index].offset, bits);
	return 1 + (bits + 7) / 8;
}

/* Packetize one 20 ms MIME storage frame. No outbound rate request (CMR=15). */
static inline int amr_payload_pack(const unsigned char *frame, size_t len,
	int wideband, int aligned, unsigned char *out, size_t capacity)
{
	int bits;
	size_t size;
	if (!frame || !len || (frame[0] & 0x80)) {
		return -1;
	}
	bits = amr_frame_bits(wideband, (frame[0] >> 3) & 15);
	if (bits < 0 || len != 1 + (size_t) (bits + 7) / 8) {
		return -1;
	}
	size = aligned ? len + 1 : (size_t) (10 + bits + 7) / 8;
	if (!out || size > capacity) {
		return -1;
	}
	memset(out, 0, size);
	out[0] = 0xf0;
	amr_bits_copy(out, aligned ? 8 : 4, frame, 0, 6);
	amr_bits_copy(out, aligned ? 16 : 10, frame, 8, bits);
	return size;
}

#endif
