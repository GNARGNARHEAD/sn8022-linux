// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SN6180 / SN8022 support for codec SSID 2014:7054.
 * Offline integration draft: board wiring, protection and recovery require
 * hardware validation. The profile is supplied separately via request_firmware.
 */
#include <crypto/sha2.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <sound/core.h>
#include <sound/hda_codec.h>
#include <sound/hda_regmap.h>
#include "hda_local.h"
#include "hda_auto_parser.h"
#include "generic.h"
#include "sn8022.h"
#include "sn8022_core.h"

#define SN8022_FIRMWARE "conexant/sn8022-20147054.bin"
#define SN8022_AMP_CAPS 0x80034a4aU

struct sn8022 {
	struct hda_codec *codec;
	/* PM reference (or codec PM callback) -> lock -> regmap -> cmd_mutex.
	 * Never call generic, vmaster or original PCM handlers under this lock.
	 */
	struct mutex lock;
	struct sn8022_io io;
	struct sn8022_state state;
	const struct firmware *firmware;
	struct hda_pcm_ops pcm_ops;
	unsigned int generation;
	struct task_struct *powered_task;
	unsigned int powered_depth;
	int init_error;
	bool ready;
	bool runtime_pending;
	bool resume_fast;
	bool prepared;
	bool output_muted;
	u8 gain[2];
	bool unmute[2]; /* effective vmaster values, before board/automute gating */
};

static bool target_matches(struct hda_codec *codec)
{
	return codec && codec->core.vendor_id == SN8022_CODEC_ID &&
		codec->core.subsystem_id == SN8022_SUBSYSTEM_ID &&
		codec->core.addr <= 0xf && codec->bus && codec->core.exec_verb;
}

static int verify_profile(void *context, const u8 *data, size_t size)
{
	static const u8 expected[SHA256_DIGEST_SIZE] = {
		0x49, 0xca, 0x40, 0x06, 0xdc, 0xa8, 0xfa, 0x84,
		0x9f, 0x31, 0x0b, 0x2b, 0x08, 0x9e, 0xfa, 0xff,
		0x9a, 0xe9, 0xa2, 0x4f, 0x24, 0x61, 0x2e, 0xbf,
		0x3e, 0x4a, 0x9d, 0x78, 0x72, 0x3d, 0xf2, 0x14,
	};
	u8 digest[SHA256_DIGEST_SIZE];

	if (!data || size != SN8022_PROFILE_SIZE)
		return -EINVAL;
	sha256(data, size, digest);
	return memcmp(digest, expected, sizeof(digest)) ? -EBADMSG : 0;
}

/* Caller owns codec power before amp->lock. No flags-losing bus fallback. */
static int hda_command(void *context, u32 word, u32 *response)
{
	struct sn8022 *amp = context;
	struct hda_codec *codec = amp->codec;

	if (!target_matches(codec))
		return -ENODEV;
	if (!response || (word & 0xf8000000U))
		return -EINVAL;
	return codec->core.exec_verb(&codec->core,
			word | ((u32)codec->core.addr << 28),
			HDA_RW_NO_RESPONSE_FALLBACK, response);
}

static int command(struct sn8022 *amp, u32 word, u32 *result)
{
	u32 response = 0;
	int err = hda_command(amp, word, &response);

	if (err)
		return err < 0 ? err : -EIO;
	if (response == ~0U)
		return -EIO;
	if (result)
		*result = response;
	return 0;
}

static void delay_ms(void *context, unsigned int milliseconds)
{
	msleep(milliseconds);
}

/* Full-byte writes intentionally force hardware I/O even when cache equals
 * the request: an earlier failed write may already have updated regcache.
 * These are the SAME per-channel pseudo registers used by generic HDA amps.
 * Full writes do not read first and reach codec_exec_verb with res=NULL:
 * its synchronous bus-reset branch is excluded. Do not replace these with
 * amp_update/read under this lock. Retain the outer checked PM reference.
 */
static int write_outputs(struct sn8022 *amp, bool force_mute)
{
	static const hda_nid_t nids[] = { 0x11, 0x22 };
	bool allowed = amp->ready && amp->prepared && !amp->output_muted && !force_mute;
	unsigned int expected[2][2];
	u32 response;
	int i, ch, err, first_error = 0;

	for (i = 0; i < ARRAY_SIZE(nids); i++) {
		for (ch = 0; ch < 2; ch++) {
			unsigned int reg = snd_hdac_regmap_encode_amp(nids[i], ch,
								 HDA_OUTPUT, 0);
			unsigned int val = amp->gain[ch];

			if (!allowed || !amp->unmute[ch] || first_error)
				val |= HDA_AMP_MUTE;
			expected[i][ch] = val;
			err = snd_hdac_regmap_write_raw(&amp->codec->core, reg, val);
			if (err < 0 && !first_error)
				first_error = err;
		}
	}
	/* These explicit reads use the checked, no-fallback command path. The
	 * standard cached writes above retain ordinary controller fallback and
	 * discard their response payload; a successful return alone is weaker.
	 */
	for (i = 0; i < ARRAY_SIZE(nids); i++) {
		for (ch = 0; ch < 2; ch++) {
			u32 word = (nids[i] << 20) |
				(AC_VERB_GET_AMP_GAIN_MUTE << 8) | AC_AMP_GET_OUTPUT |
				(ch ? AC_AMP_GET_RIGHT : AC_AMP_GET_LEFT);

			err = command(amp, word, &response);
			if (!err && (response & 0xff) != expected[i][ch])
				err = -EIO;
			if (err && !first_error)
				first_error = err;
		}
	}
	return first_error;
}

static void fault(struct sn8022 *amp, int error)
{
	amp->runtime_pending = false;
	amp->resume_fast = false;
	amp->ready = false;
	amp->prepared = false;
	/* Keep a completed profile eligible for shutdown. Playback permission
	 * is separate; only profile/power operations invalidate their state.
	 */
	amp->generation++;
	/* Best effort only: no software can promise mute after lost transport. */
	write_outputs(amp, true);
	codec_warn(amp->codec, "SN8022 speakers unavailable (%d)\n", error);
}

static int apply_outputs(struct sn8022 *amp)
{
	int err = write_outputs(amp, false);

	if (err)
		fault(amp, err);
	return err;
}

/* Only callbacks nested in THIS task's powered init or control construction
 * may omit a runtime wake. HDA builds controls before enabling runtime PM.
 * codec->in_pm is codec-wide and cannot establish caller ownership.
 */
static int power_get(struct sn8022 *amp, bool *held)
{
	int err;

	*held = READ_ONCE(amp->powered_task) != current;
	err = *held ? snd_hda_power_up(amp->codec) : 0;
	if (err < 0) {
		/* No bus I/O is permitted after a failed runtime power reference. */
		mutex_lock(&amp->lock);
		amp->runtime_pending = false;
		amp->resume_fast = false;
		amp->ready = false;
		amp->prepared = false;
		amp->generation++;
		mutex_unlock(&amp->lock);
		codec_warn(amp->codec, "SN8022 codec wake failed (%d)\n", err);
	}
	return err;
}

static void power_put(struct sn8022 *amp, bool held)
{
	if (held)
		snd_hda_power_down(amp->codec);
}

static int control_get(struct snd_kcontrol *kcontrol,
		       struct snd_ctl_elem_value *ucontrol, bool volume)
{
	struct sn8022 *amp = cx_sn8022(snd_kcontrol_chip(kcontrol));
	int ch;

	mutex_lock(&amp->lock);
	for (ch = 0; ch < 2; ch++)
		ucontrol->value.integer.value[ch] = volume ?
			amp->gain[ch] : amp->unmute[ch];
	mutex_unlock(&amp->lock);
	return 0;
}

static int control_put(struct snd_kcontrol *kcontrol,
		       struct snd_ctl_elem_value *ucontrol, bool volume)
{
	struct sn8022 *amp = cx_sn8022(snd_kcontrol_chip(kcontrol));
	int ch, err, changed = 0;
	bool held;
	long *values = ucontrol->value.integer.value;

	for (ch = 0; ch < 2; ch++)
		if (values[ch] < 0 || values[ch] > (volume ? 0x4a : 1))
			return -EINVAL;
	/* vmaster commits its logical value before invoking a follower put.
	 * Retain the corresponding effective request even if the PM wake fails.
	 */
	mutex_lock(&amp->lock);
	for (ch = 0; ch < 2; ch++) {
		if (volume) {
			changed |= amp->gain[ch] != values[ch];
			amp->gain[ch] = values[ch];
		} else {
			changed |= amp->unmute[ch] != values[ch];
			amp->unmute[ch] = values[ch];
		}
	}
	mutex_unlock(&amp->lock);
	err = power_get(amp, &held);
	if (err < 0)
		goto power_down;
	mutex_lock(&amp->lock);
	err = apply_outputs(amp);
	mutex_unlock(&amp->lock);
 power_down:
	power_put(amp, held);
	return err < 0 ? err : changed;
}

static int speaker_volume_get(struct snd_kcontrol *k, struct snd_ctl_elem_value *v)
{
	return control_get(k, v, true);
}

static int speaker_volume_put(struct snd_kcontrol *k, struct snd_ctl_elem_value *v)
{
	return control_put(k, v, true);
}

static int speaker_switch_get(struct snd_kcontrol *k, struct snd_ctl_elem_value *v)
{
	return control_get(k, v, false);
}

static int speaker_switch_put(struct snd_kcontrol *k, struct snd_ctl_elem_value *v)
{
	return control_put(k, v, false);
}

static void automute(struct hda_codec *codec)
{
	struct hda_gen_spec *gen = codec->spec;
	struct sn8022 *amp = cx_sn8022(codec);
	bool held;
	int err = power_get(amp, &held);

	if (err < 0)
		goto out;
	/* Retain ordinary pin automute and EAPD policy. With auto_mute_via_amp
	 * clear, the generic caller does not replay stale vmaster follower values.
	 */
	snd_hda_gen_update_outputs(codec);
	mutex_lock(&amp->lock);
	amp->output_muted = gen->speaker_muted;
	apply_outputs(amp);
	mutex_unlock(&amp->lock);
 out:
	power_put(amp, held);
}

static int configure_link(struct sn8022 *amp)
{
	/* Candidate ordering follows SU4 INF presentation. The Windows registry
	 * enumeration order is not established; this is a hardware review gate.
	 */
	static const u32 overrides[] = {
		0x01c1f81d, 0x000f0000, 0x01c1e038, 0x000f0000,
		0x000f0000, 0x01c1e018, 0x000f0000,
	};
	/* Exact 51-word SU4 I2S setup, independently transcribed from OEM bytes.
	 * Only input converters 0x24/25 receive raw formats here. Generic HDA
	 * exclusively owns playback format/stream registers 0x10/11/22.
	 */
	static const u32 i2s[] = {
		0x00170500, 0x01d70740, 0x01d70500, 0x02270500,
		0x01e70500, 0x01e70720, 0x02470500, 0x02420031,
		0x0243704a, 0x02670740, 0x02670500, 0x02370500,
		0x02770500, 0x02770720, 0x02570500, 0x02520031,
		0x0253704a, 0x01b63201, 0x01b6003f, 0x01b6011f,
		0x01b60232, 0x01b60344, 0x01b60417, 0x01b60600,
		0x01b60700, 0x01b60d44, 0x01b60e17, 0x01b61000,
		0x01b61100, 0x01b22c80, 0x01b6282f, 0x01b628af,
		0x01b62c03, 0x01b63301, 0x01b6143f, 0x01b6151f,
		0x01b61632, 0x01b61744, 0x01b61817, 0x01b61a00,
		0x01b61b00, 0x01b62144, 0x01b62217, 0x01b62400,
		0x01b62500, 0x01b22d80, 0x01b62d2f, 0x01b62daf,
		0x01b63103, 0x01b60c0f, 0x01b6200f,
	};
	static const u32 gpio_get[] = { 0x001f1600, 0x001f1700, 0x001f1500 };
	static const u32 gpio_set[] = { 0x00171600, 0x00171700, 0x00171500 };
	unsigned int i;
	u32 value;
	int err;

	for (i = 0; i < ARRAY_SIZE(overrides); i++) {
		err = command(amp, overrides[i], NULL);
		if (err)
			return err;
	}
	for (i = 0; i < ARRAY_SIZE(i2s); i++) {
		err = command(amp, i2s[i], NULL);
		if (err)
			return err;
	}
	/* Preserve unrelated GPIO bits; no invented reset-low pulse. */
	for (i = 0; i < ARRAY_SIZE(gpio_get); i++) {
		err = command(amp, gpio_get[i], &value);
		if (err)
			return err;
		err = command(amp, gpio_set[i] | ((value | BIT(4)) & 0xff), NULL);
		if (err)
			return err;
	}
	msleep(100);
	return 0;
}

static int configure(struct sn8022 *amp)
{
	int err = configure_link(amp);

	if (err)
		return err;
	err = sn8022_program(&amp->io, amp->firmware->data,
			     amp->firmware->size, &amp->state);
	if (!err)
		err = sn8022_set_power(&amp->io, &amp->state, true);
	return err;
}


/* Only ordinary runtime callbacks may preserve amplifier configuration.
 * The parent may have reset its link: fingerprint verification is still needed.
 */
static bool ordinary_runtime(struct sn8022 *amp)
{
	return amp->codec->core.registered &&
		snd_hdac_is_in_pm(&amp->codec->core) &&
		!amp->codec->bus->in_reset &&
		amp->codec->core.dev.power.power_state.event == PM_EVENT_ON;
}

/* Guard the electrical state before configure_link restores any GPIO bits. */
static int retained_gpio(struct sn8022 *amp)
{
	static const u32 reads[] = { 0x001f1600, 0x001f1700, 0x001f1500 };
	unsigned int i;
	u32 value;
	int err;

	for (i = 0; i < ARRAY_SIZE(reads); i++) {
		err = command(amp, reads[i], &value);
		if (err)
			return err;
		if (value > 0xff || !(value & BIT(4)))
			return -ESTALE;
	}
	return 0;
}

/* Experimental guard: live tests established these eight bytes across idle,
 * not retention of every tuning/protection register. Full setup is fallback.
 */
static int resume_retained(struct sn8022 *amp)
{
	static const u8 expected[8] = {
		0x02, 0x87, 0xa2, 0x6c, 0xf8, 0x10, 0x05, 0xa1,
	};
	u8 sample[8], observation;
	int err;

	err = retained_gpio(amp);
	if (!err)
		err = configure_link(amp);
	if (!err)
		err = sn8022_retention_sample(&amp->io, amp->firmware->data,
					      amp->firmware->size, sample, &observation);
	if (!err && memcmp(sample, expected, sizeof(expected)))
		err = -ESTALE;
	if (err)
		return err;
	/* The undocumented observation byte is not a retention gate. */
	amp->state.passes = 0;
	amp->state.initialized = true;
	return sn8022_set_power(&amp->io, &amp->state, true);
}

/* Caller holds amp->lock. The HDA core supplies the powered callback
 * context; this task identity does not confer power ownership on other tasks.
 */
static void powered_enter(struct sn8022 *amp)
{
	if (!amp->powered_depth++)
		WRITE_ONCE(amp->powered_task, current);
}

static void powered_leave(struct sn8022 *amp)
{
	if (!--amp->powered_depth)
		WRITE_ONCE(amp->powered_task, NULL);
}

/* HDA retains its construction power reference through build_controls and
 * enables runtime PM only at codec registration. Generic vmaster creation
 * calls our control puts in this interval, after the init callback returns.
 */
int sn8022_build_controls(struct hda_codec *codec)
{
	struct sn8022 *amp = cx_sn8022(codec);
	int err;

	if (!amp)
		return snd_hda_gen_build_controls(codec);
	mutex_lock(&amp->lock);
	powered_enter(amp);
	mutex_unlock(&amp->lock);

	err = snd_hda_gen_build_controls(codec);

	mutex_lock(&amp->lock);
	powered_leave(amp);
	mutex_unlock(&amp->lock);
	return err;
}

/* Init and suspend entrypoints run in the powered HDA codec callback context. */
unsigned int sn8022_init_begin(struct sn8022 *amp)
{
	unsigned int generation;

	if (!amp)
		return 0;
	mutex_lock(&amp->lock);
	powered_enter(amp);
	generation = ++amp->generation;
	amp->resume_fast = amp->runtime_pending && ordinary_runtime(amp);
	amp->runtime_pending = false;
	amp->ready = false;
	amp->prepared = false;
	amp->state.initialized = false;
	/* Before generic init/regcache replay, including cold/no-vmaster init. */
	amp->init_error = write_outputs(amp, true);
	if (amp->init_error)
		amp->resume_fast = false;
	mutex_unlock(&amp->lock);
	return generation;
}

void sn8022_init_finish(struct sn8022 *amp, unsigned int generation)
{
	const char *mode = "full";
	int err, guard_err = 0;

	if (!amp)
		return;
	mutex_lock(&amp->lock);
	if (generation != amp->generation)
		goto out;
	err = amp->init_error;
	if (!err && !amp->firmware)
		err = -ENOENT;
	if (!err && amp->resume_fast && ordinary_runtime(amp)) {
		mode = "fast";
		err = resume_retained(amp);
		if (err) {
			guard_err = err;
			mode = "fallback";
			amp->state.initialized = false;
			err = configure(amp);
		}
	} else if (!err) {
		err = configure(amp);
	}
	amp->resume_fast = false;
	if (err)
		fault(amp, err);
	else
		amp->ready = true;
	codec_info(amp->codec,
		   "SN8022 resume mode=%s generation=%u error=%d guard_error=%d\n",
		   mode, generation, err, guard_err);
 out:
	powered_leave(amp);
	mutex_unlock(&amp->lock);
}

void sn8022_suspend(struct sn8022 *amp)
{
	bool healthy;
	int err, power_err = 0;

	if (!amp)
		return;
	mutex_lock(&amp->lock);
	healthy = amp->ready && amp->state.initialized;
	amp->runtime_pending = false;
	amp->resume_fast = false;
	amp->ready = false;
	amp->prepared = false;
	amp->generation++;
	err = write_outputs(amp, true);
	if (amp->state.initialized)
		power_err = sn8022_set_power(&amp->io, &amp->state, false);
	if (healthy && !err && !power_err && ordinary_runtime(amp))
		amp->runtime_pending = true;
	amp->state.initialized = false;
	if (err || power_err)
		fault(amp, err ? err : power_err);
	mutex_unlock(&amp->lock);
}

static int verify_stream(struct sn8022 *amp, unsigned int tag, unsigned int format)
{
	static const hda_nid_t nids[] = { 0x11, 0x22 };
	u32 value;
	int i, err;

	for (i = 0; i < ARRAY_SIZE(nids); i++) {
		err = command(amp, (nids[i] << 20) | 0xf0600, &value);
		if (err || (value & 0xff) != (tag << 4))
			return err ? err : -EIO;
		err = command(amp, (nids[i] << 20) | 0xa0000, &value);
		if (err || (value & 0xffff) != format)
			return err ? err : -EIO;
	}
	return 0;
}

static int playback_prepare(struct hda_pcm_stream *info, struct hda_codec *codec,
			    unsigned int tag, unsigned int format,
			    struct snd_pcm_substream *substream)
{
	struct sn8022 *amp = cx_sn8022(codec);
	unsigned int generation;
	int err, amp_err;
	bool held;

	if (substream->runtime->channels != 2 || tag < 1 || tag > 15 ||
	    (format != 0x0011 && format != 0x0031))
		return -EINVAL;
	err = power_get(amp, &held);
	if (err < 0)
		goto power_down;
	mutex_lock(&amp->lock);
	amp->prepared = false;
	amp_err = write_outputs(amp, true);
	if (amp_err)
		fault(amp, amp_err);
	generation = amp->generation;
	mutex_unlock(&amp->lock);

	err = amp->pcm_ops.prepare(info, codec, tag, format, substream);

	mutex_lock(&amp->lock);
	if (!err && generation == amp->generation && amp->ready) {
		amp_err = verify_stream(amp, tag, format);
		if (amp_err)
			fault(amp, amp_err);
		else {
			amp->prepared = true;
			apply_outputs(amp);
		}
	}
	mutex_unlock(&amp->lock);
	/* Amp failure does not destroy the shared headphone PCM. */
 power_down:
	power_put(amp, held);
	return err;
}

static int playback_cleanup(struct hda_pcm_stream *info, struct hda_codec *codec,
			    struct snd_pcm_substream *substream)
{
	struct sn8022 *amp = cx_sn8022(codec);
	bool held;
	int err, cleanup_err;

	/* The outer HDA cleanup ignores errors: invalidate before attempting PM. */
	mutex_lock(&amp->lock);
	amp->prepared = false;
	amp->generation++;
	mutex_unlock(&amp->lock);
	err = power_get(amp, &held);
	if (err >= 0) {
		mutex_lock(&amp->lock);
		apply_outputs(amp);
		mutex_unlock(&amp->lock);
	}
	/* Preserve generic converter bookkeeping even if our PM wake failed.
	 * Any immediate generic hardware cleanup uses the core's own PM helpers.
	 */
	cleanup_err = amp->pcm_ops.cleanup(info, codec, substream);
	power_put(amp, held);
	return err < 0 ? err : cleanup_err;
}

int sn8022_build_pcms(struct sn8022 *amp)
{
	struct hda_gen_spec *gen;
	struct hda_pcm_stream *pcm;

	if (!amp)
		return 0;
	gen = amp->codec->spec;
	if (!gen->pcm_rec[0])
		return -EINVAL;
	pcm = &gen->pcm_rec[0]->stream[SNDRV_PCM_STREAM_PLAYBACK];
	if (!pcm->ops.prepare || !pcm->ops.cleanup)
		return -EINVAL;
	if (pcm->ops.prepare == playback_prepare)
		return 0;
	amp->pcm_ops = pcm->ops;
	pcm->ops.prepare = playback_prepare;
	pcm->ops.cleanup = playback_cleanup;
	pcm->channels_min = pcm->channels_max = 2;
	pcm->rates = SNDRV_PCM_RATE_48000;
	pcm->formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S32_LE;
	pcm->maxbps = 24;
	pcm->chmap = snd_pcm_std_chmaps;
	return 0;
}

/* Transform templates before vmaster replaces public callbacks/private_data.
 * Preserve both paths' ctl associations so generic init won't unmute them.
 */
int sn8022_parse(struct sn8022 *amp)
{
	struct hda_gen_spec *gen;
	struct snd_kcontrol_new *k, *controls;
	struct nid_path *path;
	unsigned int i, found = 0, dst = 0, bit;
	static const hda_nid_t nids[] = { 0x11, 0x22 };

	if (!amp)
		return 0;
	gen = amp->codec->spec;
	if (gen->autocfg.line_out_type != AUTO_PIN_SPEAKER_OUT ||
	    gen->autocfg.line_outs != 2 || gen->multiout.num_dacs != 2 ||
	    gen->multiout.dac_nids[0] != 0x11 || gen->multiout.dac_nids[1] != 0x22 ||
	    gen->multiout.hp_nid || gen->multiout.hp_out_nid[0] != 0x10 ||
	    gen->multiout.hp_out_nid[1] || gen->multiout.extra_out_nid[0] ||
	    gen->autocfg.line_out_pins[0] != 0x17 ||
	    gen->autocfg.line_out_pins[1] != 0x1d ||
	    gen->autocfg.hp_outs != 1 || gen->autocfg.hp_pins[0] != 0x16 ||
	    gen->multi_ios || gen->automute_hook || gen->auto_mute_via_amp ||
	    gen->indep_hp || gen->alt_dac_nid || gen->power_down_unused ||
	    amp->codec->power_save_node)
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(nids); i++) {
		unsigned int val = HDA_COMPOSE_AMP_VAL(nids[i], 3, 0, HDA_OUTPUT);
		unsigned int wcaps = get_wcaps(amp->codec, nids[i]);

		if (get_wcaps_type(wcaps) != AC_WID_AUD_OUT ||
		    !(wcaps & AC_WCAP_STEREO) || (wcaps & AC_WCAP_DIGITAL) ||
		    query_amp_caps(amp->codec, nids[i], HDA_OUTPUT) != SN8022_AMP_CAPS)
			return -EINVAL;
		path = snd_hda_get_path_from_idx(amp->codec, gen->out_paths[i]);
		if (!path || path->depth != 2 || path->path[0] != nids[i] ||
		    path->path[1] != (i ? 0x1d : 0x17) ||
		    path->ctls[NID_PATH_VOL_CTL] != val ||
		    path->ctls[NID_PATH_MUTE_CTL] != val)
			return -EINVAL;
	}
	path = snd_hda_get_path_from_idx(amp->codec, gen->hp_paths[0]);
	if (!path || path->depth != 2 || path->path[0] != 0x10 ||
	    path->path[1] != 0x16 ||
	    get_wcaps_type(get_wcaps(amp->codec, 0x10)) != AC_WID_AUD_OUT ||
	    !(get_wcaps(amp->codec, 0x10) & AC_WCAP_STEREO) ||
	    (get_wcaps(amp->codec, 0x10) & AC_WCAP_DIGITAL))
		return -EINVAL;
	for (i = 0; i < gen->paths.used; i++) {
		path = snd_array_elem(&gen->paths, i);
		if (path->depth < 0 || path->depth > MAX_NID_PATH_DEPTH)
			return -EINVAL;
		if (path->depth > 0 && (path->path[path->depth - 1] == 0x24 ||
				       path->path[path->depth - 1] == 0x25))
			return -EINVAL;
	}
	controls = gen->kctls.list;
	/* Validate the complete shape before any mutation. */
	for (i = 0; i < gen->kctls.used; i++) {
		unsigned int nid, expected;

		k = &controls[i];
		nid = get_amp_nid_(k->private_value);
		if (nid != 0x11 && nid != 0x22 && nid != 0x10)
			continue;
		expected = HDA_COMPOSE_AMP_VAL(nid, 3, 0, HDA_OUTPUT);
		if (k->private_value != expected || k->index)
			return -EINVAL;
		if (!strcmp(k->name, nid == 0x11 ? "Speaker Playback Volume" :
			    nid == 0x22 ? "Bass Speaker Playback Volume" :
			    "Headphone Playback Volume")) {
			if (k->put != snd_hda_mixer_amp_volume_put ||
			    k->get != snd_hda_mixer_amp_volume_get ||
			    k->info != snd_hda_mixer_amp_volume_info)
				return -EINVAL;
			bit = nid == 0x11 ? 1 : nid == 0x22 ? 4 : 16;
		} else if (!strcmp(k->name, nid == 0x11 ? "Speaker Playback Switch" :
				   nid == 0x22 ? "Bass Speaker Playback Switch" :
				   "Headphone Playback Switch")) {
			if (!k->put || k->get != snd_hda_mixer_amp_switch_get ||
			    k->info != snd_hda_mixer_amp_switch_info)
				return -EINVAL;
			bit = nid == 0x11 ? 2 : nid == 0x22 ? 8 : 32;
		} else {
			return -EINVAL;
		}
		if (found & bit)
			return -EINVAL;
		found |= bit;
	}
	if (found != 63)
		return -EINVAL;
	for (i = 0; i < gen->kctls.used; i++) {
		k = &controls[i];
		if (get_amp_nid_(k->private_value) == 0x22) {
			kfree(k->name);
			continue;
		}
		if (get_amp_nid_(k->private_value) == 0x11) {
			bool volume = k->put == snd_hda_mixer_amp_volume_put;

			k->get = volume ? speaker_volume_get : speaker_switch_get;
			k->put = volume ? speaker_volume_put : speaker_switch_put;
		}
		controls[dst++] = *k;
	}
	/* snd_hda_add_new_ctls walks a NULL-name sentinel, not array.used. */
	memset(&controls[dst], 0, (gen->kctls.used - dst) * sizeof(*controls));
	gen->kctls.used = dst;
	gen->min_channel_count = gen->ext_channel_count = gen->const_channel_count = 2;
	gen->multiout.max_channels = 2;
	gen->multiout.no_share_stream = 0;
	gen->no_multi_io = 1;
	gen->automute_hook = automute;
	return 0;
}

struct sn8022 *sn8022_create(struct hda_codec *codec)
{
	static const hda_nid_t preferred_dacs[] = { 0x17, 0x11, 0x1d, 0x22,
						  0x16, 0x10, 0 };
	struct hda_gen_spec *gen = codec->spec;
	struct sn8022 *amp;
	int err;

	if (!target_matches(codec))
		return ERR_PTR(-ENODEV);
	/* Don't silently reinterpret a retasked or different board topology. */
	if (snd_hda_codec_get_pincfg(codec, 0x17) != 0x91170110 ||
	    snd_hda_codec_get_pincfg(codec, 0x1d) != 0x40f001f0 ||
	    snd_hda_codec_get_pincfg(codec, 0x16) != 0x03211040 ||
	    query_amp_caps(codec, 0x11, HDA_OUTPUT) != SN8022_AMP_CAPS ||
	    query_amp_caps(codec, 0x22, HDA_OUTPUT) != SN8022_AMP_CAPS)
		return ERR_PTR(-EINVAL);
	amp = kzalloc(sizeof(*amp), GFP_KERNEL);
	if (!amp)
		return ERR_PTR(-ENOMEM);
	amp->codec = codec;
	mutex_init(&amp->lock);
	amp->output_muted = true;
	amp->io.context = amp;
	amp->io.command = hda_command;
	amp->io.sleep_ms = delay_ms;
	amp->io.verify_profile = verify_profile;
	err = request_firmware(&amp->firmware, SN8022_FIRMWARE, &codec->core.dev);
	if (!err)
		err = verify_profile(NULL, amp->firmware->data, amp->firmware->size);
	if (err) {
		release_firmware(amp->firmware);
		amp->firmware = NULL;
		codec_warn(codec, "SN8022 profile unavailable/invalid (%d)\n", err);
	}
	/* Linux parser metadata, not an observed BIOS/Windows pin default. */
	err = snd_hda_codec_set_pincfg(codec, 0x1d, 0x91170111);
	if (err < 0) {
		release_firmware(amp->firmware);
		kfree(amp);
		return ERR_PTR(err);
	}
	gen->preferred_dacs = preferred_dacs;
	gen->no_multi_io = 1;
	gen->auto_mute_via_amp = 0;
	return amp;
}

void sn8022_free(struct sn8022 *amp)
{
	if (!amp)
		return;
	release_firmware(amp->firmware);
	kfree(amp);
}

MODULE_FIRMWARE(SN8022_FIRMWARE);
