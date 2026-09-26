// SPDX-License-Identifier: GPL-2.0-or-later
/* Recovered SN8022 bridge protocol; caller supplies transport and profile. */
#ifndef SN8022_CORE_H
#define SN8022_CORE_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint32_t u32;
#endif

#define SN8022_CODEC_ID 0x14f120d1U
#define SN8022_SUBSYSTEM_ID 0x20147054U
#define SN8022_PROFILE_SIZE 2982U
#define SN8022_MAX_PASSES 12U

/* The caller serializes the whole operation and holds codec power throughout.
 * command() completes one HDA request, returns 0 or a negative errno, and
 * stores the response separately. Words have a zero codec-address nibble.
 * verify_profile() must authenticate the actual bytes against the exact SU4
 * SHA-256 before any command. All callbacks must be present.
 */
struct sn8022_io {
	void *context;
	int (*command)(void *context, u32 word, u32 *response);
	void (*sleep_ms)(void *context, unsigned int milliseconds);
	int (*verify_profile)(void *context, const u8 *data, size_t size);
};

struct sn8022_state {
	u8 cached_observation;
	bool initialized;
	unsigned int passes;
};

/* Implements only the recovered bridge/profile subsection. GPIO, I2S,
 * overrides, clocks and stream synchronization remain the caller's work.
 * "initialized" means the OEM consistency comparison passed, NOT that
 * protection or physical playback has been validated.
 */
int sn8022_program(const struct sn8022_io *io, const u8 *data, size_t size,
		   struct sn8022_state *state);
int sn8022_set_power(const struct sn8022_io *io, struct sn8022_state *state,
		     bool d0);

/* Partial profile fingerprint; not a complete configuration checksum. */
int sn8022_retention_sample(const struct sn8022_io *io,
			    const u8 *profile, size_t size,
			    u8 values[8], u8 *observation);

#endif
