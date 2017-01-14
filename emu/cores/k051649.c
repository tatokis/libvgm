/***************************************************************************

    Konami 051649 - SCC1 sound as used in Haunted Castle, City Bomber

    This file is pieced together by Bryan McPhail from a combination of
    Namco Sound, Amuse by Cab, Haunted Castle schematics and whoever first
    figured out SCC!

    The 051649 is a 5 channel sound generator, each channel gets its
    waveform from RAM (32 bytes per waveform, 8 bit signed data).

    This sound chip is the same as the sound chip in some Konami
    megaROM cartridges for the MSX. It is actually well researched
    and documented:

        http://bifi.msxnet.org/msxnet/tech/scc.html

    Thanks to Sean Young (sean@mess.org) for some bugfixes.

    K052539 is more or less equivalent to this chip except channel 5
    does not share waveram with channel 4.

***************************************************************************/

#include <stdlib.h>
#include <string.h>	// for memset

#include <stdtype.h>
#include "../EmuStructs.h"
#include "../EmuCores.h"
#include "../snddef.h"
#include "../EmuHelper.h"
#include "k051649.h"

typedef struct _k051649_state k051649_state;

static void k051649_update(void *param, UINT32 samples, DEV_SMPL **outputs);
static UINT8 device_start_k051649(const DEV_GEN_CFG* cfg, DEV_INFO* retDevInf);
static void device_stop_k051649(void *chip);
static void device_reset_k051649(void *chip);

static void k051649_waveform_w(void *chip, UINT8 offset, UINT8 data);
static UINT8 k051649_waveform_r(void *chip, UINT8 offset);
static void k051649_volume_w(void *chip, UINT8 offset, UINT8 data);
static void k051649_frequency_w(void *chip, UINT8 offset, UINT8 data);
static void k051649_keyonoff_w(void *chip, UINT8 offset, UINT8 data);

static void k052539_waveform_w(void *chip, UINT8 offset, UINT8 data);
static UINT8 k052539_waveform_r(void *chip, UINT8 offset);

static void k051649_test_w(void *chip, UINT8 offset, UINT8 data);
static UINT8 k051649_test_r(void *chip, UINT8 offset);

static void k051649_w(void *chip, UINT8 offset, UINT8 data);
static UINT8 k051649_r(void *chip, UINT8 offset, UINT8 data);

static void k051649_set_mute_mask(void *chip, UINT32 MuteMask);


static DEVDEF_RWFUNC devFunc[] =
{
	{RWF_REGISTER | RWF_WRITE, DEVRW_A8D8, 0, k051649_w},
	{RWF_REGISTER | RWF_READ, DEVRW_A8D8, 0, k051649_r},
	{0x00, 0x00, 0, NULL}
};
static DEV_DEF devDef =
{
	"K051649", "MAME", FCC_MAME,
	
	device_start_k051649,
	device_stop_k051649,
	device_reset_k051649,
	k051649_update,
	
	NULL,	// SetOptionBits
	k051649_set_mute_mask,
	NULL,	// SetPanning
	NULL,	// SetSampleRateChangeCallback
	NULL,	// LinkDevice
	
	devFunc,	// rwFuncs
};

const DEV_DEF* devDefList_K051649[] =
{
	&devDef,
	NULL
};


#define FREQ_BITS	16
#define DEF_GAIN	8

/* this structure defines the parameters for a channel */
typedef struct
{
	UINT32 counter;
	INT32 frequency;
	UINT8 volume;
	UINT8 key;
	INT8 waveram[32];		/* 19991207.CAB */
	UINT8 Muted;
} k051649_sound_channel;

struct _k051649_state
{
	void* chipInf;
	
	k051649_sound_channel channel_list[5];

	/* global sound parameters */
	UINT32 mclock,rate;

	/* mixer tables and internal buffers */
	INT16 *mixer_table;
	INT16 *mixer_lookup;
	short *mixer_buffer;

	UINT8 cur_reg;
	UINT8 test;
	UINT8 mode_plus;
};

/* build a table to divide by the number of voices */
static void make_mixer_table(k051649_state *info, int voices)
{
	int count = voices * 256;
	int i;

	/* allocate memory */
	info->mixer_table = (INT16*)malloc(sizeof(INT16) * 2 * count);

	/* find the middle of the table */
	info->mixer_lookup = info->mixer_table + count;

	/* fill in the table - 16 bit case */
	for (i = 0; i < count; i++)
	{
		int val = i * DEF_GAIN * 16 / voices;
		if (val > 32767) val = 32767;
		info->mixer_lookup[ i] = val;
		info->mixer_lookup[-i] = -val;
	}
}


/* generate sound to the mix buffer */
static void k051649_update(void *param, UINT32 samples, DEV_SMPL **outputs)
{
	k051649_state *info = (k051649_state *)param;
	k051649_sound_channel *voice=info->channel_list;
	DEV_SMPL *buffer = outputs[0];
	DEV_SMPL *buffer2 = outputs[1];
	short *mix;
	UINT32 i,j;

	// zap the contents of the mixer buffer
	memset(info->mixer_buffer, 0, samples * sizeof(short));

	for (j=0; j<5; j++) {
		// channel is halted for freq < 9
		if (voice[j].frequency > 8 && ! voice[j].Muted)
		{
			const INT8 *w = voice[j].waveram;			/* 19991207.CAB */
			INT16 v=voice[j].volume * voice[j].key;
			UINT32 c=voice[j].counter;
			/* Amuse source:  Cab suggests this method gives greater resolution */
			/* Sean Young 20010417: the formula is really: f = clock/(16*(f+1))*/
			//UINT32 step = (UINT32)(((INT64)info->mclock * (1 << FREQ_BITS)) / (float)((voice[j].frequency + 1) * 16 * (info->rate / 32)) + 0.5);
			UINT32 step = (UINT32)(((INT64)info->mclock * (1 << FREQ_BITS)) / (float)((voice[j].frequency + 1) * (info->rate / 2)) + 0.5);

			mix = info->mixer_buffer;

			// add our contribution
			for (i = 0; i < samples; i++)
			{
				UINT32 offs;

				c += step;
				offs = (c >> FREQ_BITS) & 0x1f;
				*mix++ += (w[offs] * v)>>3;
			}

			// update the counter for this voice
			voice[j].counter = c;
		}
	}

	// mix it down
	mix = info->mixer_buffer;
	for (i = 0; i < samples; i++)
		*buffer++ = *buffer2++ = info->mixer_lookup[*mix++];
}

static UINT8 device_start_k051649(const DEV_GEN_CFG* cfg, DEV_INFO* retDevInf)
{
	k051649_state *info;

	info = (k051649_state *)calloc(1, sizeof(k051649_state));
	if (info == NULL)
		return 0xFF;

	/* get stream channels */
	info->mode_plus = CHPCLK_FLAG(cfg->clock);
	info->mclock = CHPCLK_CLOCK(cfg->clock);
	info->rate = info->mclock / 16;

	/* allocate a buffer to mix into - 1 second's worth should be more than enough */
	info->mixer_buffer = (short*)malloc(sizeof(short) * info->rate);

	/* build the mixer table */
	make_mixer_table(info, 5);
	
	k051649_set_mute_mask(info, 0x00);
	
	info->chipInf = info;
	INIT_DEVINF(retDevInf, (DEV_DATA*)info, info->rate, &devDef);
	return 0x00;
}

static void device_stop_k051649(void *chip)
{
	k051649_state *info = (k051649_state *)chip;
	
	free(info->mixer_buffer);
	free(info->mixer_table);
	free(info);
	
	return;
}

static void device_reset_k051649(void *chip)
{
	k051649_state *info = (k051649_state *)chip;
	k051649_sound_channel *voice = info->channel_list;
	int i;

	// reset all the voices
	for (i = 0; i < 5; i++)
	{
		voice[i].frequency = 0;
		voice[i].volume = 0;
		voice[i].counter = 0;
		voice[i].key = 0;
	}
	
	// other parameters
	info->test = 0x00;
	info->cur_reg = 0x00;
	
	return;
}

/********************************************************************************/

static void k051649_waveform_w(void *chip, UINT8 offset, UINT8 data)
{
	k051649_state *info = (k051649_state *)chip;
	
	// waveram is read-only?
	if (info->test & 0x40 || (info->test & 0x80 && offset >= 0x60))
		return;
	
	if (offset >= 0x60)
	{
		// channel 5 shares waveram with channel 4
		info->channel_list[3].waveram[offset&0x1f]=data;
		info->channel_list[4].waveram[offset&0x1f]=data;
	}
	else
		info->channel_list[offset>>5].waveram[offset&0x1f]=data;
}

static UINT8 k051649_waveform_r(void *chip, UINT8 offset)
{
	k051649_state *info = (k051649_state *)chip;
	
	// test-register bits 6/7 expose the internal counter
	if (info->test & 0xc0)
	{
		if (offset >= 0x60)
			offset += (info->channel_list[3 + (info->test >> 6 & 1)].counter >> FREQ_BITS);
		else if (info->test & 0x40)
			offset += (info->channel_list[offset>>5].counter >> FREQ_BITS);
	}
	return info->channel_list[offset>>5].waveram[offset&0x1f];
}

/* SY 20001114: Channel 5 doesn't share the waveform with channel 4 on this chip */
static void k052539_waveform_w(void *chip, UINT8 offset, UINT8 data)
{
	k051649_state *info = (k051649_state *)chip;
	
	// waveram is read-only?
	if (info->test & 0x40)
		return;

	info->channel_list[offset>>5].waveram[offset&0x1f]=data;
}

static UINT8 k052539_waveform_r(void *chip, UINT8 offset)
{
	k051649_state *info = (k051649_state *)chip;
	
	// test-register bit 6 exposes the internal counter
	if (info->test & 0x40)
	{
		offset += (info->channel_list[offset>>5].counter >> FREQ_BITS);
	}
	return info->channel_list[offset>>5].waveram[offset&0x1f];
}

static void k051649_volume_w(void *chip, UINT8 offset, UINT8 data)
{
	k051649_state *info = (k051649_state *)chip;
	info->channel_list[offset&0x7].volume=data&0xf;
}

static void k051649_frequency_w(void *chip, UINT8 offset, UINT8 data)
{
	k051649_state *info = (k051649_state *)chip;
	k051649_sound_channel* chn = &info->channel_list[offset >> 1];
	
	// test-register bit 5 resets the internal counter
	if (info->test & 0x20)
		chn->counter = ~0;
	else if (chn->frequency < 9)
		chn->counter |= ((1 << FREQ_BITS) - 1);

	// update frequency
	if (offset & 1)
		chn->frequency = (chn->frequency & 0x0FF) | ((data << 8) & 0xF00);
	else
		chn->frequency = (chn->frequency & 0xF00) |  (data << 0);
	chn->counter &= 0xFFFF0000;	// Valley Bell: Behaviour according to openMSX
}

static void k051649_keyonoff_w(void *chip, UINT8 offset, UINT8 data)
{
	k051649_state *info = (k051649_state *)chip;
	int i;
	
	for (i = 0; i < 5; i++)
	{
		info->channel_list[i].key=data&1;
		data >>= 1;
	}
}

static void k051649_test_w(void *chip, UINT8 offset, UINT8 data)
{
	k051649_state *info = (k051649_state *)chip;
	info->test = data;
}


static UINT8 k051649_test_r(void *chip, UINT8 offset)
{
	// reading the test register sets it to $ff!
	k051649_test_w(chip, offset, 0xff);
	return 0xff;
}


static void k051649_w(void *chip, UINT8 offset, UINT8 data)
{
	k051649_state *info = (k051649_state *)chip;
	
	switch(offset & 1)
	{
	case 0x00:
		info->cur_reg = data;
		break;
	case 0x01:
		switch(offset >> 1)
		{
		case 0x00:
		case 0x04:
			if (! info->mode_plus)
				k051649_waveform_w(chip, info->cur_reg, data);
			else
				k052539_waveform_w(chip, info->cur_reg, data);
			break;
		case 0x01:
			k051649_frequency_w(chip, info->cur_reg, data);
			break;
		case 0x02:
			k051649_volume_w(chip, info->cur_reg, data);
			break;
		case 0x03:
			k051649_keyonoff_w(chip, info->cur_reg, data);
			break;
		case 0x05:
			k051649_test_w(chip, info->cur_reg, data);
			break;
		}
		break;
	}
	
	return;
}

static UINT8 k051649_r(void *chip, UINT8 offset, UINT8 data)
{
	k051649_state *info = (k051649_state *)chip;
	
	switch(offset >> 1)
	{
	case 0x00:
	case 0x04:
		if (! info->mode_plus)
			return k051649_waveform_r(chip, info->cur_reg);
		else
			return k052539_waveform_r(chip, info->cur_reg);
	case 0x01:
		return 0xFF;
	case 0x02:
		return 0xFF;
	case 0x03:
		return 0xFF;
	case 0x05:
		return k051649_test_r(chip, info->cur_reg);
	default:
		return 0x00;
	}
}


static void k051649_set_mute_mask(void *chip, UINT32 MuteMask)
{
	k051649_state *info = (k051649_state *)chip;
	UINT8 CurChn;
	
	for (CurChn = 0; CurChn < 5; CurChn ++)
		info->channel_list[CurChn].Muted = (MuteMask >> CurChn) & 0x01;
	
	return;
}