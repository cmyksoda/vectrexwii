#include <math.h>
#include <string.h>
#include "vecx.h"
#include "psg.h"

#define einline __inline

/* software model of the vectrex's AY-3-8912 PSG. vecx_emu() reports elapsed
 * cycles once per 6809 instruction via psg_run(), and samples are batch
 * generated at 48kHz -- one sample every 31.25 cpu cycles (1.5MHz / 48kHz).
 * counters are kept in cpu-cycle units so the output is identical to
 * stepping every cycle, without the per-cycle call overhead.
 * snd_regs[0..13] (vecx.c) are read directly here -- registers 14/15 are the
 * AY's io port (controller input) and are not part of the synth.
 */

enum {
	/* cpu cycles per output sample = CYCFRAC_NUM / CYCFRAC_DEN = 31.25.
	 * sixteenth-cycle units give the host's rate tuner 0.2% resolution,
	 * far below audible pitch steps.
	 */
	CYCFRAC_NUM    = 500,
	CYCFRAC_DEN    = 16,

	OVERSAMPLE     = 4,    /* synth runs at 4x 48kHz to suppress square-edge aliasing */

	/* A tone channel's flip-flop TOGGLES when its counter expires, so one full
	 * square-wave cycle spans TWO reloads. To land on the datasheet frequency
	 * fT = clock/(16*TP) the toggle interval must be 8*TP cpu cycles, not 16*TP
	 * (the latter halved every tone -- an octave too low). Noise instead SHIFTS
	 * its LFSR once per reload (no half-cycle), so it keeps the full 16*NP.
	 */
	TONE_PRESCALE  = 8,    /* tone half-period: toggle every 8*TP cpu cycles */
	NOISE_PRESCALE = 16,   /* noise LFSR shift every 16*NP cpu cycles */
	ENV_PRESCALE   = 256,  /* envelope period is in 256-cycle units */
	ENV_STEPS      = 16,   /* AY-3-8910/8912 envelope resolution (32 is the YM2149) */
	CHAN_MAXAMP    = 8000  /* peak per channel; sum of 3 leaves headroom so the dc
	                        * blocker's transients don't hit the s16 rails (clip) */
};

static long tone_counter[3]; /* cpu cycles until next half-period toggle */
static unsigned tone_out[3];

static long noise_counter;
static unsigned noise_lfsr;
static unsigned noise_out;

static long env_counter;
static int env_step;
static unsigned env_holding;
static unsigned env_hold_level;
static unsigned env_dir_invert;
static unsigned env_level;

static long cyc_pending;  /* emulated cycles not yet turned into samples */
static int cyc_frac;      /* bresenham remainder for the fractional cycle */
static int cyc_num;       /* sixteenth-cycles per output sample; 500 nominal,
                           * host may trim it to rate-match the audio clock */
static long sub_acc;      /* oversampling accumulator */
static int sub_count;

/* analog output path of the real console: the PSG output is ac-coupled and
 * then through a small speaker with little response above a few kHz. the dc
 * blocker also absorbs level steps from the +-level mixing.
 */
static float flt_dcx, flt_dcy;
static float flt_lp1;

static short sample_buf[PSG_MAX_SAMPLES];
static int sample_count;

static short vol_table[16];
static short env_table[ENV_STEPS];

#ifdef VECX_TRACE
int vecx_trace_env_level = 0;   /* current envelope step (0..15) */
int vecx_trace_env_holding = 0; /* has the envelope reached its hold state? */
#endif

/* advance the envelope generator by one step (called once per envelope
 * period reload). shape truth table per the AY-3-8910/8912 datasheet:
 * Continue=0 always settles at 0; Continue=1,Hold=1 settles at 0 or max
 * depending on Attack^Alternate; Continue=1,Hold=0 repeats, flipping
 * direction each cycle when Alternate is set.
 */

static einline void env_advance (void)
{
	unsigned shape    = snd_regs[13];
	unsigned attack    = (shape & 0x04) != 0;
	unsigned alternate = (shape & 0x02) != 0;
	unsigned hold      = (shape & 0x01) != 0;
	unsigned cont      = (shape & 0x08) != 0;

	if (env_holding) {
		return;
	}

	env_step++;

	if (env_step >= ENV_STEPS) {
		if (!cont) {
			env_holding = 1;
			env_hold_level = 0;
		} else if (hold) {
			env_holding = 1;
			env_hold_level = (attack ^ alternate) ? (ENV_STEPS - 1) : 0;
		} else {
			env_step = 0;
			if (alternate) {
				env_dir_invert ^= 1;
			}
		}
	}

	env_level = env_holding ? env_hold_level
	          : ((attack ^ env_dir_invert) ? (unsigned) env_step : (ENV_STEPS - 1 - (unsigned) env_step));

#ifdef VECX_TRACE
	vecx_trace_env_level = (int) env_level;
	vecx_trace_env_holding = (int) env_holding;
#endif
}

/* mix the 3 channels for the current instant. each channel contributes
 * +-level (not 0/level) so the result is centered on 0 like the real chip's
 * ac-coupled output, rather than sitting on a dc offset.
 */

static einline long mix_channels (void)
{
	unsigned mixer = snd_regs[7];
	long sum = 0;
	int ch;

	for (ch = 0; ch < 3; ch++) {
		unsigned amp = snd_regs[8 + ch];
		unsigned level = (amp & 0x10) ? (unsigned) env_table[env_level] : (unsigned) vol_table[amp & 0x0f];
		unsigned tone_term  = (mixer & (1u << ch))       ? 1u : tone_out[ch];
		unsigned noise_term = (mixer & (1u << (ch + 3))) ? 1u : noise_out;

		/* unipolar 0..level like the real chip -- bipolar mixing doubles the
		 * transient of every frame-rate register change into an audible
		 * 50Hz click layer. the dc blocker downstream recenters the output.
		 */
		sum += (tone_term && noise_term) ? (long) level : 0;
	}

	return sum;
}

/* filter an averaged (already anti-aliased) sample through the analog path
 * model and push it to the output buffer.
 */

static einline void emit_sample (long sum)
{
	{
		/* dc blocker (~10Hz highpass) then a gentle one-pole lowpass
		 * (~8kHz) approximating the console's speaker rolloff.
		 */
		float x = (float) sum;
		float y = x - flt_dcx + 0.99869f * flt_dcy;

		flt_dcx = x;
		flt_dcy = y;

		flt_lp1 += 0.70f * (y - flt_lp1);

		sum = (long) flt_lp1;
	}

	if (sum > 32767) {
		sum = 32767;
	} else if (sum < -32768) {
		sum = -32768;
	}

	if (sample_count < PSG_MAX_SAMPLES) {
		sample_buf[sample_count++] = (short) sum;
	}
}

#ifdef VECX_TRACE
/* snapshot/restore of the envelope generator state, which a write to R13
 * (via psg_env_restart) mutates -- lets the differential tester roll the PSG
 * back after the reference core executes an instruction.
 */
static struct { long env_counter; int env_step; unsigned env_holding,
	env_hold_level, env_dir_invert, env_level; } psgsav;
void psg_snapshot (void)
{
	psgsav.env_counter = env_counter; psgsav.env_step = env_step;
	psgsav.env_holding = env_holding; psgsav.env_hold_level = env_hold_level;
	psgsav.env_dir_invert = env_dir_invert; psgsav.env_level = env_level;
}
void psg_restore (void)
{
	env_counter = psgsav.env_counter; env_step = psgsav.env_step;
	env_holding = psgsav.env_holding; env_hold_level = psgsav.env_hold_level;
	env_dir_invert = psgsav.env_dir_invert; env_level = psgsav.env_level;
}
#endif

void psg_reset (void)
{
	int ch, i;

	for (ch = 0; ch < 3; ch++) {
		tone_counter[ch] = TONE_PRESCALE;
		tone_out[ch] = 0;
	}

	noise_counter = NOISE_PRESCALE;
	noise_lfsr = 1; /* must never settle at 0 or the lfsr locks up */
	noise_out = 0;

	env_counter = ENV_PRESCALE;
	env_step = -1;
	env_holding = 0;
	env_hold_level = 0;
	env_dir_invert = 0;
	env_level = 0;

	cyc_pending = 0;
	cyc_frac = 0;
	cyc_num = CYCFRAC_NUM;
	sub_acc = 0;
	sub_count = 0;

	flt_dcx = 0;
	flt_dcy = 0;
	flt_lp1 = 0;

	sample_count = 0;

	/* measured AY-3-8910 dac response (normalized amplitudes per volume
	 * level, from hardware measurements) -- the envelope drives the same
	 * dac, so both tables share the curve.
	 */
	{
		static const float dac[16] = {
			0.0f,     0.00999f, 0.01445f, 0.02103f,
			0.03070f, 0.04554f, 0.06422f, 0.10745f,
			0.12602f, 0.20864f, 0.27452f, 0.34584f,
			0.44485f, 0.57460f, 0.74491f, 1.0f
		};

		for (i = 0; i < 16; i++) {
			vol_table[i] = (short) (CHAN_MAXAMP * dac[i] + 0.5f);
			env_table[i] = vol_table[i];
		}
	}
}

/* the real chip restarts the envelope generator whenever the shape register
 * (R13) is written, even with the same value -- games rely on this to
 * retrigger percussive effects like explosions.
 */

void psg_env_restart (void)
{
	unsigned period = (snd_regs[12] << 8) | snd_regs[11];

	if (period == 0) {
		period = 1;
	}

	env_counter = (long) period * ENV_PRESCALE;
	env_step = -1;
	env_holding = 0;
	env_hold_level = 0;
	env_dir_invert = 0;

	env_advance ();
}

/* adjust the cycles-per-sample ratio by delta_q sixteenth-cycles (0.2% per
 * step). the host tunes this until sample production matches what the audio
 * hardware actually consumes -- important because emulated audio clocks
 * (dolphin's in particular) don't run at exactly the nominal 48kHz. tuning
 * the ratio keeps the stream gapless AND self-corrects pitch: waveform
 * periods are defined in cpu cycles, so producing more samples per cycle
 * exactly compensates a faster consumer.
 */

void psg_rate_trim (int delta_q)
{
	if (delta_q > 100) delta_q = 100;
	if (delta_q < -100) delta_q = -100;

	cyc_num = CYCFRAC_NUM + delta_q;
}

void psg_run (unsigned cycles)
{
	cyc_pending += (long) cycles;

	/* each iteration advances one SUBsample (1/4 of an output sample); the
	 * guard tracks cyc_num so dc can never exceed the pending budget.
	 */
	while (cyc_pending >= (cyc_num / (CYCFRAC_DEN * OVERSAMPLE)) + 1) {
		long dc, period;
		int ch;

		cyc_frac += cyc_num;
		dc = cyc_frac / (CYCFRAC_DEN * OVERSAMPLE);
		cyc_frac %= (CYCFRAC_DEN * OVERSAMPLE);

		cyc_pending -= dc;

		for (ch = 0; ch < 3; ch++) {
			tone_counter[ch] -= dc;

			while (tone_counter[ch] <= 0) {
				period = (long) (((snd_regs[2 * ch + 1] & 0x0f) << 8) | snd_regs[2 * ch]);

				if (period == 0) {
					period = 1;
				}

				tone_counter[ch] += period * TONE_PRESCALE;
				tone_out[ch] ^= 1;
			}
		}

		noise_counter -= dc;

		while (noise_counter <= 0) {
			period = (long) (snd_regs[6] & 0x1f);

			if (period == 0) {
				period = 1;
			}

			noise_counter += period * NOISE_PRESCALE;

			if (noise_lfsr & 1) {
				noise_lfsr ^= 0x24000;
			}
			noise_lfsr >>= 1;
			noise_out = noise_lfsr & 1;
		}

		env_counter -= dc;

		while (env_counter <= 0) {
			period = (long) ((snd_regs[12] << 8) | snd_regs[11]);

			if (period == 0) {
				period = 1;
			}

			env_counter += period * ENV_PRESCALE;

			env_advance ();
		}

		/* accumulate subsamples; every OVERSAMPLE-th, average and emit */
		sub_acc += mix_channels ();

		if (++sub_count >= OVERSAMPLE) {
			emit_sample (sub_acc / OVERSAMPLE);
			sub_acc = 0;
			sub_count = 0;
		}
	}
}

int psg_read_samples (short *out, int max_samples)
{
	int n = sample_count;

	if (n > max_samples) {
		n = max_samples;
	}

	memcpy (out, sample_buf, (size_t) n * sizeof (short));

	sample_count = 0;

	return n;
}
