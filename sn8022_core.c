// SPDX-License-Identifier: GPL-2.0-or-later
/* Recovered command construction, with explicit transport errors.
 * Evidence: analysis/PROTOCOL.md, OEM sample identified there by SHA-256.
 * There is no hardware backend in this file.
 */
#include "sn8022_core.h"
#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#endif

static bool valid_io(const struct sn8022_io *io)
{
	return io && io->command && io->sleep_ms && io->verify_profile;
}

static int command(const struct sn8022_io *io, u32 word, u32 *response)
{
	u32 value = 0;
	int ret = io->command(io->context, word, &value);

	/* A response cannot carry an errno. Keep the channels separate. */
	if (ret)
		return ret < 0 ? ret : -EIO;
	if (value == 0xffffffffU)
		return -EIO;
	if (response)
		*response = value;
	return 0;
}

static int sequence(const struct sn8022_io *io, const u32 *words, size_t count)
{
	size_t i;
	int ret;

	for (i = 0; i < count; i++) {
		ret = command(io, words[i], NULL);
		if (ret)
			return ret;
	}
	return 0;
}

static int setup(const struct sn8022_io *io)
{
	const u32 words[] = { 0x01b52080, 0x01b5212d };

	return sequence(io, words, 2);
}

/* OEM RVA 0x6394: six commands and 44 vendor-ID requests per byte. */
static int bulk_write(const struct sn8022_io *io, u8 reg, u8 value)
{
	u32 words[50];
	unsigned int i;

	words[0] = 0x01b52a00 | value;
	words[1] = 0x01b52600;
	words[2] = 0x01b52700 | reg;
	words[3] = 0x01b52800;
	words[4] = 0x01b52901;
	words[5] = 0x01b52902;
	for (i = 6; i < 50; i++)
		words[i] = 0x000f0000;
	return sequence(io, words, 50);
}

/* OEM 0x8898 + 0x5708. This READ also WRITES two selectors. */
static int read_observation(const struct sn8022_io *io, u8 *observation)
{
	u32 words[50], response;
	unsigned int i;
	int ret;

	ret = bulk_write(io, 0x00, 0x00);
	if (ret)
		return ret;
	io->sleep_ms(io->context, 10);
	ret = bulk_write(io, 0x7f, 0x00);
	if (ret)
		return ret;
	io->sleep_ms(io->context, 10);
	words[0] = 0x01b52600;
	words[1] = 0x01b5277e;
	words[2] = 0x01b52810;
	words[3] = 0x01b52901;
	words[4] = 0x01b52902;
	for (i = 5; i < 50; i++)
		words[i] = 0x000f0000;
	ret = sequence(io, words, 50);
	if (ret)
		return ret;
	ret = command(io, 0x01bd2a00, &response);
	if (!ret)
		*observation = (u8)response;
	return ret;
}

int sn8022_program(const struct sn8022_io *io, const u8 *data, size_t size,
		   struct sn8022_state *state)
{
	u8 previous = 0, observed;
	size_t i;
	int ret;

	if (!state)
		return -EINVAL;
	state->initialized = false;
	state->passes = 0;
	if (!valid_io(io) || !data || size != SN8022_PROFILE_SIZE)
		return -EINVAL;
	ret = io->verify_profile(io->context, data, size);
	if (ret)
		return ret < 0 ? ret : -EINVAL;
	ret = setup(io);
	if (ret)
		return ret;
	while (state->passes < SN8022_MAX_PASSES) {
		state->passes++;
		for (i = 0; i < size; i += 2) {
			ret = bulk_write(io, data[i], data[i + 1]);
			if (ret)
				return ret;
		}
		ret = read_observation(io, &observed);
		if (ret)
			return ret;
		if ((state->cached_observation &&
		     observed == state->cached_observation) ||
		    (!state->cached_observation && state->passes > 1 &&
		     observed && observed == previous)) {
			state->cached_observation = observed;
			state->initialized = true;
			return 0;
		}
		previous = observed;
	}
	return -EIO;
}

/* OEM RVA 0x60b4/0x9d70. One attempt, with completion/error masks.
 * As a Linux-side improvement, an attempted trigger is followed by a clear
 * even when its transport call or status read fails. This is not proven
 * hardware recovery; failure invalidates initialization below.
 */
static int runtime_write(const struct sn8022_io *io, u8 reg, u8 value)
{
	const u32 words[] = { 0x01b52600, 0x01b52700 | reg,
			      0x01b52a00 | value, 0x01b52800 };
	u32 status;
	int ret, clear_ret;

	ret = sequence(io, words, 4);
	if (ret)
		return ret;
	ret = command(io, 0x01b52901, NULL);
	if (!ret) {
		io->sleep_ms(io->context, 1);
		ret = command(io, 0x01bd2900, &status);
		if (!ret && !(status & 2)) {
			io->sleep_ms(io->context, 2);
			ret = -ETIMEDOUT;
		} else if (!ret && (status & 4)) {
			ret = -EIO;
		}
	}
	clear_ret = command(io, 0x01b52902, NULL);
	if (!ret)
		ret = clear_ret;
	if (ret)
		io->sleep_ms(io->context, 2);
	return ret;
}

int sn8022_set_power(const struct sn8022_io *io, struct sn8022_state *state,
		     bool d0)
{
	const u8 non_d0[] = { 0, 0, 0x7f, 0, 0, 0, 3, 0 };
	const u8 to_d0[] = { 0, 0, 0x7f, 0, 0, 0, 0x0d, 1,
			     0, 0, 0x7f, 0, 0, 0, 3, 3 };
	const u8 *pairs = d0 ? to_d0 : non_d0;
	size_t i, size = d0 ? sizeof(to_d0) : sizeof(non_d0);
	int ret;

	if (!valid_io(io) || !state)
		return -EINVAL;
	if (!state->initialized)
		return -EAGAIN;
	ret = setup(io);
	for (i = 0; !ret && i < size; i += 2)
		ret = runtime_write(io, pairs[i], pairs[i + 1]);
	if (ret)
		state->initialized = false;
	return ret;
}

/* Parameterized OEM 0x5708 byte read, preserving padding. */
static int retention_read_byte(const struct sn8022_io *io, u8 reg, u8 *value)
{
	u32 words[50], response;
	unsigned int i;
	int err;

	words[0] = 0x01b52600;
	words[1] = 0x01b52700 | reg;
	words[2] = 0x01b52810;
	words[3] = 0x01b52901;
	words[4] = 0x01b52902;
	for (i = 5; i < 50; i++)
		words[i] = 0x000f0000;
	err = sequence(io, words, 50);
	if (!err)
		err = command(io, 0x01bd2a00, &response);
	if (!err)
		*value = (u8)response;
	return err;
}

static int retention_selector(const struct sn8022_io *io, u8 reg, u8 value)
{
	int err = bulk_write(io, reg, value);

	/* Conservatively use the recovered base-selector delay for all selectors. */
	io->sleep_ms(io->context, 10);
	return err;
}

int sn8022_retention_sample(const struct sn8022_io *io,
			    const u8 *profile, size_t size,
			    u8 values[8], u8 *observation)
{
	/* Inferred book/page addressing. Every selector/value is present in
	 * the exact profile; samples are not a documented protection checksum.
	 */
	static const u8 groups[][3] = {
		{ 0x8c, 0x2c, 0x64 },
		{ 0xaa, 0x26, 0x64 },
	};
	unsigned int group, byte;
	int err, restore_page, restore_book;

	if (!valid_io(io) || !profile || size != SN8022_PROFILE_SIZE ||
	    !values || !observation)
		return -EINVAL;
	err = io->verify_profile(io->context, profile, size);
	if (err)
		return err < 0 ? err : -EINVAL;
	err = setup(io);
	if (err)
		return err; /* No selectors touched; bridge setup did not complete. */
	for (group = 0; !err && group < 2; group++) {
		err = retention_selector(io, 0x00, 0x00);
		if (!err)
			err = retention_selector(io, 0x7f, groups[group][0]);
		if (!err)
			err = retention_selector(io, 0x00, groups[group][1]);
		for (byte = 0; !err && byte < 4; byte++)
			err = retention_read_byte(io, (u8)(groups[group][2] + byte),
					  &values[group * 4 + byte]);
	}
	/* Restore base selectors after an incomplete sample. Only select the
	 * base book if the page-zero request succeeded. The
	 * caller keeps outputs muted until its complete wake and PCM preparation
	 * succeed; a sample alone never authorizes playback.
	 */
	restore_page = retention_selector(io, 0x00, 0x00);
	restore_book = restore_page ? restore_page :
		retention_selector(io, 0x7f, 0x00);
	if (!err)
		err = restore_page ? restore_page : restore_book;
	if (!err)
		err = read_observation(io, observation);
	return err;
}
