#ifndef __PSG_H
#define __PSG_H

enum {
	PSG_SAMPLE_RATE = 48000, /* wii native rate -- no resampling in ASND */
	PSG_MAX_SAMPLES = 2048   /* > one EMU_TIMER (20ms) tick worth of samples */
};

void psg_reset (void);
void psg_run (unsigned cycles);
void psg_env_restart (void);
void psg_rate_trim (int delta_q);
int  psg_read_samples (short *out, int max_samples);

#endif
