/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef __HDA_SN8022_H
#define __HDA_SN8022_H

struct hda_codec;
struct sn8022;

/* Private to the Conexant module; the generic spec remains its first member. */
struct sn8022 *cx_sn8022(struct hda_codec *codec);
struct sn8022 *sn8022_create(struct hda_codec *codec);
int sn8022_parse(struct sn8022 *amp);
int sn8022_build_controls(struct hda_codec *codec);
int sn8022_build_pcms(struct sn8022 *amp);
unsigned int sn8022_init_begin(struct sn8022 *amp);
void sn8022_init_finish(struct sn8022 *amp, unsigned int generation);
void sn8022_suspend(struct sn8022 *amp);
void sn8022_free(struct sn8022 *amp);

#endif
