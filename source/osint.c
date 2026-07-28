#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include <gccore.h>
#include <wiiuse/wpad.h>
#include <fat.h>
#include <dirent.h>
#include <ogc/lwp_watchdog.h>
#include <asndlib.h>
#include <grrlib.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include "osint.h"
#include "vecx.h"
#include "visuals.h"
#include "psg.h"
#include "rom_catalog.h"
#include "credits.h"

#include "rom_dat.h" //Preloaded Minestorm
#include "Minestorm_png.h" //Minestorm overlay

#include "splashscreen_png.h"
#include "base_png.h" //Still frame of Mine Storm, used to preview the display options
#include "Font_ttf.h"

#define EMU_TIMER 20 /* the emulators heart beats at 20 milliseconds */
#define FONTHEAD 36 /* Font sizes */
#define FONTOPT 32
#define FONTROM 38

/* Screen-size (overscan) scaling: shrink drawn geometry toward the screen
 * centre. s is optScreenSize/255 (1.0 = full). */
#define SCR_CX 320.0f
#define SCR_CY 240.0f
#define SCALE_X(x, s) ((u16)(SCR_CX + ((f32)(x) - SCR_CX) * (s)))
#define SCALE_Y(y, s) ((u16)(SCR_CY + ((f32)(y) - SCR_CY) * (s)))

u8 emustatus, joystick;

/* Set false to skip the (expensive) vector draw for one frame. The renderer
 * runs inside vecx_emu, so skipping it when the audio ring runs low keeps
 * sample production real-time and stops the music from gapping/detuning. */
static u8 renderThisFrame = 1;

/* Non-zero while the options screen is open from the TITLE menu rather than
 * from an in-game pause. PauseMenu() serves both: the option rows and their
 * sub-pages are identical, so this flag just swaps the backdrop and turns the
 * "Resume Game"/"Turn vectrex OFF" rows into a plain "Back". */
static u8 settingsFromTitle;

GRRLIB_texImg *splash, *overlay;
GRRLIB_ttfFont *myFont;

/* Backdrop for the options screen when it's reached from the title menu. Kept
 * separate from `overlay` so opening the settings can never disturb the
 * overlay a loaded game owns, and loaded only while that screen is open --
 * together they are ~1.5MB of contiguous texture that would otherwise sit in
 * the heap for the whole session. */
static GRRLIB_texImg *preview, *previewOvl;

/* Overlay geometry. The overlay art is 388x480 and sits at OVL_X so that its
 * transparent window lands over the 358x445 vector area at (offx, offy). The
 * options preview still (base_png) is the same size and position, so both
 * follow one scale factor and can never drift apart. */
#define OVL_X 126.0f
#define OVL_Y 0.0f

static long scl_factor;
static long offx;
static long offy;

/* PSG samples accumulate in a ring and are pulled by ASND's per-voice
 * callback, which fires from the audio interrupt once per DSP DMA block.
 * Feeding from the interrupt instead of the emulation tick means a long
 * tick can't starve playback. The ring is lock-free single-producer/
 * single-consumer: the emu tick owns snd_widx, the interrupt owns snd_ridx,
 * both free-running (ring size power of two).
 *
 * CHUNK SIZE IS NOT ARBITRARY. ASND's DMA block is 1024 frames (4096 bytes),
 * i.e. 21.333ms at 48kHz, and the voice callback fires once per block. The
 * chunk handed over must therefore be exactly 1024 samples. A 960-sample chunk
 * (a tidy 20ms, and 32-byte aligned, which is why it looks plausible) leaves
 * the DSP 64 samples short EVERY block -- a 1.33ms dropout ~47 times a
 * second. That is inaudible on noisy SFX but shreds sustained music, and shows
 * up as a measured consumer rate of 960/21.333ms = 45000 instead of 48000.
 * 1024 samples = 2048 bytes, still a clean 32-byte multiple.
 */
#define SND_CHUNK_SAMPLES 1024
#define SND_NCHUNKS 4
#define SND_RING_SIZE 16384
#define SND_PREBUFFER 6

static short snd_ring[SND_RING_SIZE];
static volatile u32 snd_widx, snd_ridx;
static short *snd_chunk[SND_NCHUNKS]; //rotating aligned DMA buffers: one playing, one queued, rest free
static int snd_chunk_idx;
static volatile u8 snd_running;
static volatile u32 snd_underruns;
static u32 snd_drops; //diagnostics, shown on the profiler overlay

/* the audio consumer's true clock rate is MEASURED on the wall clock, from
 * the voice callback's own pulls: N consecutive chunk-pulls over T timebase
 * ticks = the DSP's real consumption rate, independent of whether production
 * kept up. (Measuring consumption per EMULATION tick instead, and only
 * trusting underrun-free windows, cannot recover once the rate is wrong enough
 * to gap every few seconds: no window is ever clean, so the wrong rate never
 * corrects itself. The wall-clock method calibrates from exactly the stretches
 * of continuous playback that exist between gaps.)
 *
 * the slope match alone never corrects the buffer LEVEL, so drift ends in
 * underruns (gaps) or ring-full drops (crackle). a one-step-per-measurement
 * (0.2%, inaudible) servo steers the backlog to the prebuffer target.
 * a starved run resets the pull counter; the learned rate persists.
 */
#define SND_RATE_MIN_PULLS 150 /* ~3s of continuous playback per measurement */
static int snd_rate_delta;
static volatile u64 snd_run_t0, snd_run_tlast; /* first/last pull timestamps of the current run */
static volatile u32 snd_run_pulls;
static volatile u8 snd_run_reset; /* producer asks the callback to start a fresh run */

#ifndef DISABLE_SOUND
/* Copy the next chunk out of the ring into a DMA-safe buffer (irq or main).
 * Does NOT advance the ring read index -- callers commit with
 * snd_commit_chunk() only once ASND actually accepted the buffer, so a
 * refused AddVoice can never silently discard 20ms of music.
 */
static short* snd_fill_chunk(){
	short *buf = snd_chunk[snd_chunk_idx];
	u32 i;

	for (i = 0; i < SND_CHUNK_SAMPLES; i++)
		buf[i] = snd_ring[(snd_ridx + i) & (SND_RING_SIZE - 1)];

	DCFlushRange(buf, SND_CHUNK_SAMPLES * sizeof(short)); //ASND reads via DMA, not through the CPU cache
	return buf;
}

static void snd_commit_chunk(){
	snd_ridx += SND_CHUNK_SAMPLES;
	snd_chunk_idx = (snd_chunk_idx + 1) % SND_NCHUNKS;
}

/* Runs in audio-interrupt context. Per asndlib source, this fires on every
 * mix cycle while the voice's second buffer slot is empty -- and when a voice
 * fully runs out it fires ONE final time and then never again (the voice
 * dies), which is why starvation must hand control back to the producer.
 */
static void snd_voice_cb(s32 voice){
	if (!snd_running) return;

	if (snd_widx - snd_ridx >= SND_CHUNK_SAMPLES) {
		if (ASND_AddVoice(0, snd_fill_chunk(), SND_CHUNK_SAMPLES * sizeof(short)) == SND_OK) {
			u64 now = gettime();
			snd_commit_chunk();

			//timestamp the pull for the wall-clock rate measurement
			if (snd_run_reset) { snd_run_pulls = 0; snd_run_reset = 0; }
			if (snd_run_pulls == 0) snd_run_t0 = now;
			snd_run_tlast = now;
			snd_run_pulls++;
		}
		/* AddVoice refused (slot still full): not starvation -- the ring is
		 * fine and this callback will simply fire again when the slot frees.
		 * Nothing was consumed thanks to the uncommitted fill.
		 */
		return;
	}

	/* Starved. Don't keep playing balanced on the empty edge -- every jitter
	 * would punch another 20ms hole in the music. Stop feeding; the producer
	 * rebuilds the full prebuffer and restarts the voice: one brief pause
	 * instead of machine-gun gaps (and it also revives the dead voice).
	 */
	snd_underruns++;
	snd_run_pulls = 0;
	snd_running = 0;
}
#endif

static void sound_init(){
#ifndef DISABLE_SOUND
	int i;
	for (i = 0; i < SND_NCHUNKS; i++)
		snd_chunk[i] = (short*) memalign(32, SND_CHUNK_SAMPLES * sizeof(short));

	ASND_Init();
	ASND_Pause(0);
#endif
}

static void sound_stop(){
#ifndef DISABLE_SOUND
	snd_running = 0; //makes the callback a no-op before the voice goes away
	ASND_StopVoice(0);
	snd_widx = 0;
	snd_ridx = 0;
	snd_run_pulls = 0; //restart the rate measurement run (learned rate is kept)
#endif
}

//Drains whatever psg_run() built up this tick into the ring; the interrupt callback does the feeding
static void osint_playaudio(){
#ifndef DISABLE_SOUND
	static short tickbuf[PSG_MAX_SAMPLES];
	int n = psg_read_samples(tickbuf, PSG_MAX_SAMPLES);
	u32 backlog = snd_widx - snd_ridx;
	u32 space = SND_RING_SIZE - backlog;
	int i;

	if (snd_running) {
		if (!snd_run_reset && snd_run_pulls >= SND_RATE_MIN_PULLS) {
			u32 p1, p2;
			u64 t0, tl;

			/* consistent snapshot: the timestamps only move on a pull, so
			 * re-read until the pull count is stable around them (u64 reads
			 * are not atomic on this cpu).
			 */
			do { p1 = snd_run_pulls; t0 = snd_run_t0; tl = snd_run_tlast; p2 = snd_run_pulls; } while (p1 != p2);

			{
				u32 ms = (u32) ticks_to_millisecs(tl - t0);
				if (ms > 0) {
					/* p1-1 chunk intervals between the first and last pull */
					u32 sps = (u32) ((u64) (p1 - 1) * SND_CHUNK_SAMPLES * 1000 / ms);

					if (sps > 43000 && sps < 53000) { /* sanity: within 10% of nominal */
						int level_err;

						/* sixteenth-cycles per sample to make production match
						 * the measured consumption exactly */
						snd_rate_delta = (int) (((u64) VECTREX_MHZ * 16 + sps / 2) / sps) - 500;

						/* level servo: slope-matching alone lets the backlog
						 * drift into the empty wall (gaps) or the full wall
						 * (drops). one 0.2% step per measurement toward the
						 * prebuffer target, +-1 chunk deadband via int division.
						 */
						level_err = ((int) backlog - SND_PREBUFFER * SND_CHUNK_SAMPLES) / SND_CHUNK_SAMPLES;
						if (level_err > 0) snd_rate_delta += 1;      /* too full: produce less */
						else if (level_err < 0) snd_rate_delta -= 1; /* too empty: produce more */
					}
				}
			}

			snd_run_reset = 1; //next pull starts a fresh measurement run
		}

		if (snd_rate_delta > 100) snd_rate_delta = 100;
		if (snd_rate_delta < -100) snd_rate_delta = -100;
		psg_rate_trim(snd_rate_delta);
	}

	if (n > (int) space) { snd_drops += n - space; n = (int) space; }

	for (i = 0; i < n; i++)
		snd_ring[(snd_widx + i) & (SND_RING_SIZE - 1)] = tickbuf[i];
	snd_widx += n;

	if (!snd_running && snd_widx - snd_ridx >= SND_PREBUFFER * SND_CHUNK_SAMPLES) {
		short *buf = snd_fill_chunk();
		snd_run_pulls = 0; //fresh measurement run (the voice isn't live yet, no irq race)
		snd_run_reset = 0;
		snd_running = 1; //set before SetVoice so an immediate callback isn't ignored
		if (ASND_SetVoice(0, VOICE_MONO_16BIT, PSG_SAMPLE_RATE, 0, buf, SND_CHUNK_SAMPLES * sizeof(short), 255, 255, snd_voice_cb) == SND_OK)
			snd_commit_chunk();
		else
			snd_running = 0; //couldn't start; retry next tick
	}
#endif
}

// The drawing area for vectors is 358x445 starting at (offx, offy)
void osint_render(){
u32 i, j, v, c;

if(!renderThisFrame) return; //frame-skipped to keep audio real-time; the beam's vectors are swapped by the caller regardless

if (optVtxCustomColor[0]) c = RGBA(optVtxCustomColor[1], optVtxCustomColor[2], optVtxCustomColor[3], 255);
else c = (vectors_draw[0].color * 256 / VECTREX_COLORS*0x1010100)+0xFF;

		if(persFull) free(vectors_pers[persCycle]); //Free the current position pointer, which is the oldest frame

		vectors_pers[persCycle] = (wii_vector_t*) malloc (sizeof(wii_vector_t)*vector_draw_cnt); //allocate memory to hold the coordinates of every vector in the current frame
		if(vectors_pers[persCycle] == NULL){ vector_pers_cnt[persCycle] = 0; return; } //out of memory: drop the frame rather than write through NULL
		vector_pers_cnt[persCycle] = vector_draw_cnt; //Remember the number of vectors drawn at this frame

		//Store the coordinates of the current frame in the persistence array
		//These are used from now (to avoid computing them again), scaled for overscan
		{
			f32 s = optScreenSize / 255.0f;
			for(v = 0; v < vector_draw_cnt; v++){
					vectors_pers[persCycle][v].x0 = SCALE_X(offx + vectors_draw[v].x0 / scl_factor, s);
					vectors_pers[persCycle][v].y0 = SCALE_Y(offy + vectors_draw[v].y0 / scl_factor, s);
					vectors_pers[persCycle][v].x1 = SCALE_X(offx + vectors_draw[v].x1 / scl_factor, s);
					vectors_pers[persCycle][v].y1 = SCALE_Y(offy + vectors_draw[v].y1 / scl_factor, s);
			}
		}

		//This draws the old vectors [persistence]
		if(optPersistence[0] && persFull)
			for(i = 1; i <= optPersistence[1]; i++){
			j = (persCycle >= i ? persCycle - i : PERSFRAMES - i + persCycle + 1);
				for(v = 0; v < vector_pers_cnt[j]; v++){
					vbatch_point(vectors_pers[j][v].x0, vectors_pers[j][v].y0, RGBA(optPersistence[2], optPersistence[2], optPersistence[2], optPersistence[3]));
					vbatch_line(vectors_pers[j][v].x0, vectors_pers[j][v].y0, vectors_pers[j][v].x1, vectors_pers[j][v].y1, RGBA(optPersistence[2], optPersistence[2], optPersistence[2], optPersistence[3]));
				}
			}
		vbatch_flush(); //must land under the persistence pass's blend mode before it changes below

		//Blurred vectors to give a glowing effect
		if(optGlow[0]){
			u32 glowc = (c & 0xFFFFFF00) | optGlow[2]; //same colour as the vectors, glow-opacity alpha instead of opaque
			GRRLIB_SetBlend(GRRLIB_BLEND_ADD);
			for(v = 0; v < vector_draw_cnt; v++){
				blurDot(vectors_pers[persCycle][v].x0, vectors_pers[persCycle][v].y0, optGlow[1]/7 + 2, optGlow[1], glowc);
				blurDot(vectors_pers[persCycle][v].x1, vectors_pers[persCycle][v].y1, optGlow[1]/7 + 2, optGlow[1], glowc);
				blurLine(vectors_pers[persCycle][v].x0, vectors_pers[persCycle][v].y0, vectors_pers[persCycle][v].x1, vectors_pers[persCycle][v].y1, optGlow[1], glowc);
			}
			vbatch_flush(); //must land under GRRLIB_BLEND_ADD before it's switched back below
			GRRLIB_SetBlend(GRRLIB_BLEND_ALPHA);
		}

		//Draw the current vectors normally
		for(v = 0; v < vector_draw_cnt; v++){
			vbatch_point(vectors_pers[persCycle][v].x0, vectors_pers[persCycle][v].y0, c);
			vbatch_line(vectors_pers[persCycle][v].x0, vectors_pers[persCycle][v].y0, vectors_pers[persCycle][v].x1, vectors_pers[persCycle][v].y1, c);
		}
		vbatch_flush();

		if (++persCycle > PERSFRAMES) {persCycle = 0; persFull = 1;} //wrap around and set the flag to start freeing old data

		{
			f32 s = optScreenSize / 255.0f;
			if(overlay != NULL && optOverlay[0]) GRRLIB_DrawImg(SCR_CX + (OVL_X - SCR_CX) * s, SCR_CY + (OVL_Y - SCR_CY) * s, overlay, 0, s, s, 0xFFFFFF00+optOverlay[1]);
			else GRRLIB_Rectangle(SCALE_X(offx, s), SCALE_Y(offy, s), 358.0f * s, 445.0f * s, c, 0);
		}

	GRRLIB_Render();
}

//Render the screen as it was when the emulator was paused
void pause_render(){
u32 i, j, v, c;
f32 s = optScreenSize / 255.0f;

	if (optVtxCustomColor[0]) c = RGBA(optVtxCustomColor[1], optVtxCustomColor[2], optVtxCustomColor[3], 255);
	else c = (vectors_erse[0].color * 256 / VECTREX_COLORS*0x1010100)+0xFF;

		if(optPersistence[0] && persFull)
			for(i = 2; i <= optPersistence[1] + 1; i++){
			j = (persCycle >= i ? persCycle - i : PERSFRAMES - i + persCycle + 1);
				for(v = 0; v < vector_pers_cnt[j]; v++){
					vbatch_point(vectors_pers[j][v].x0, vectors_pers[j][v].y0, RGBA(optPersistence[2], optPersistence[2], optPersistence[2], optPersistence[3]));
					vbatch_line(vectors_pers[j][v].x0, vectors_pers[j][v].y0, vectors_pers[j][v].x1, vectors_pers[j][v].y1, RGBA(optPersistence[2], optPersistence[2], optPersistence[2], optPersistence[3]));
				}
			}
		vbatch_flush(); //must land under the persistence pass's blend mode before it changes below

		if(optGlow[0]){
			u32 glowc = (c & 0xFFFFFF00) | optGlow[2]; //same colour as the vectors, glow-opacity alpha instead of opaque
			GRRLIB_SetBlend(GRRLIB_BLEND_ADD);
			for(v = 0; v < vector_erse_cnt; v++){
				blurDot(SCALE_X(offx + vectors_erse[v].x0 / scl_factor, s), SCALE_Y(offy + vectors_erse[v].y0 / scl_factor, s), optGlow[1]/7 + 2, optGlow[1], glowc);
				blurDot(SCALE_X(offx + vectors_erse[v].x1 / scl_factor, s), SCALE_Y(offy + vectors_erse[v].y1 / scl_factor, s), optGlow[1]/7 + 2, optGlow[1], glowc);
				blurLine(SCALE_X(offx + vectors_erse[v].x0 / scl_factor, s), SCALE_Y(offy + vectors_erse[v].y0 / scl_factor, s), SCALE_X(offx + vectors_erse[v].x1 / scl_factor, s), SCALE_Y(offy + vectors_erse[v].y1 / scl_factor, s), optGlow[1], glowc);
			}
			vbatch_flush(); //must land under GRRLIB_BLEND_ADD before it's switched back below
			GRRLIB_SetBlend(GRRLIB_BLEND_ALPHA);
		}

		for(v = 0; v < vector_erse_cnt; v++){
			vbatch_point(SCALE_X(offx + vectors_erse[v].x0 / scl_factor, s), SCALE_Y(offy + vectors_erse[v].y0 / scl_factor, s), c);
			vbatch_line(SCALE_X(offx + vectors_erse[v].x0 / scl_factor, s), SCALE_Y(offy + vectors_erse[v].y0 / scl_factor, s), SCALE_X(offx + vectors_erse[v].x1 / scl_factor, s), SCALE_Y(offy + vectors_erse[v].y1 / scl_factor, s), c);
		}
		vbatch_flush();


		if(overlay != NULL && optOverlay[0]) GRRLIB_DrawImg(SCR_CX + (OVL_X - SCR_CX) * s, SCR_CY + (OVL_Y - SCR_CY) * s, overlay, 0, s, s, 0xFFFFFF00+optOverlay[1]);
		else GRRLIB_Rectangle(SCALE_X(offx, s), SCALE_Y(offy, s), 358.0f * s, 445.0f * s, c, 0);
}

/* Backdrop for the options screen when it's opened from the title menu: there
 * is no paused game to freeze, so stand in a still frame of Mine Storm and put
 * the real overlay over it. Opacity, custom colour and screen size then all
 * preview live, which is the whole point of reaching the options before a ROM
 * is loaded. (Glow and persistence are motion effects and can't be shown on a
 * still, so they stay off here.) */
static void preview_render(){
f32 s = optScreenSize / 255.0f;
f32 x = SCR_CX + (OVL_X - SCR_CX) * s, y = SCR_CY + (OVL_Y - SCR_CY) * s;
u32 c;

	//The still is white-on-black, so modulating it by the custom colour tints
	//the vectors exactly as the live renderer would
	if (optVtxCustomColor[0]) c = RGBA(optVtxCustomColor[1], optVtxCustomColor[2], optVtxCustomColor[3], 255);
	else c = 0xFFFFFFFF;

	if(preview != NULL) GRRLIB_DrawImg(x, y, preview, 0, s, s, c);
	if(previewOvl != NULL && optOverlay[0]) GRRLIB_DrawImg(x, y, previewOvl, 0, s, s, 0xFFFFFF00+optOverlay[1]);
}

static void previewLoad(){
	if(preview == NULL)    preview    = GRRLIB_LoadTexture(base_png);
	if(previewOvl == NULL) previewOvl = GRRLIB_LoadTexture(Minestorm_png);
}
static void previewFree(){
	if(preview != NULL)    { GRRLIB_FreeTexture(preview);    preview = NULL; }
	if(previewOvl != NULL) { GRRLIB_FreeTexture(previewOvl); previewOvl = NULL; }
}

/* ---- centred text -----------------------------------------------------
 * Text is centred everywhere with (fbWidth - GRRLIB_WidthTTF(...))/2. That
 * width is the sum of the glyph ADVANCES, but PrintfTTF draws each glyph's
 * ink at pen + bitmap_left -- so the visible ink sits inside the advance box
 * by the first glyph's left bearing plus the last glyph's right bearing.
 * Both vary with the string (a title ending in ')' loses ~5px at FONTROM,
 * one ending in 'm' barely 1px), which is why some ROM names looked centred
 * and others visibly did not. Measure the real ink extent instead. */
static int centerXTTF(const char *str, unsigned size){
	FT_Face face = (myFont != NULL) ? (FT_Face) myFont->face : NULL;
	int pen = 0, first = 0, last = 0, seen = 0;
	const unsigned char *p;

	if (face != NULL && FT_Set_Pixel_Sizes(face, 0, size) == 0)
		for (p = (const unsigned char *) str; *p != '\0'; p++) {
			if (FT_Load_Char(face, *p, FT_LOAD_RENDER) != 0) continue;
			if (face->glyph->bitmap.width > 0) {
				if (!seen) { first = pen + face->glyph->bitmap_left; seen = 1; }
				last = pen + face->glyph->bitmap_left + face->glyph->bitmap.width;
			}
			pen += face->glyph->advance.x >> 6;
		}

	if (!seen) return ((int) rmode->fbWidth - (int) GRRLIB_WidthTTF(myFont, str, size)) / 2;
	return ((int) rmode->fbWidth - (last - first)) / 2 - first;
}

static void PrintCenteredTTF(int y, const char *str, unsigned size, u32 color){
	GRRLIB_PrintfTTF(centerXTTF(str, size), y, myFont, str, size, color);
}

/* ROM titles come from the bundled catalogue AND from arbitrary SD filenames,
 * so their width is unbounded. GRRLIB rasterises a whole string into a single
 * texture and one wider than the screen simply never appeared -- that was the
 * "long names show up blank" bug. Shrink to fit, then ellipsise as a last
 * resort, so no filename can make the title vanish again. */
#define TITLE_MAXW 600 /* a small margin inside the 640px frame */
static void PrintRomTitle(int y, const char *str, f32 s){
	/* Fitting costs several width measurements and each one rasterises the
	 * whole string, so remember the result: it only changes when the browse
	 * index moves or the Screen Size setting does, not on every one of the 60
	 * frames a second. The whole title shrinks with the rest of the screen
	 * (Screen Size), so the fit width and start font scale by s too. */
	static char src[160], fitted[160];
	static unsigned size;
	static int x;
	static f32 lastS = -1.0f;

	if (lastS != s || strncmp(src, str, sizeof(src) - 1) != 0) {
		int n;
		int maxw = (int)(TITLE_MAXW * s);
		unsigned floor = (unsigned)(20 * s);

		lastS = s;
		strncpy(src, str, sizeof(src) - 1); src[sizeof(src) - 1] = '\0';
		strcpy(fitted, src);

		size = (unsigned)(FONTROM * s + 0.5f);
		while (size > floor && GRRLIB_WidthTTF(myFont, fitted, size) > maxw) size -= 2;

		n = (int) strlen(fitted);
		while (n > 4 && GRRLIB_WidthTTF(myFont, fitted, size) > maxw) {
			n--;
			memcpy(fitted + n - 3, "...", 4);
		}

		x = centerXTTF(fitted, size);
	}

	GRRLIB_PrintfTTF(x, y, myFont, fitted, size, 0xADFF2FFF);
}

/* A slider's caption plus its current value, drawn as one centred block. The
 * layout is measured from a fixed reference string ("Opacity 100%") so the
 * caption stays put as the digits change width instead of sliding about. */
static void drawSliderCaption(int y, const char *label, u8 value){
	char ref[24], pct[8];
	int x;

	snprintf(ref, sizeof(ref), "%s 100%%", label);
	snprintf(pct, sizeof(pct), "%d%%", value * 100 / 255);
	x = centerXTTF(ref, FONTOPT);

	GRRLIB_PrintfTTF(x, y, myFont, label, FONTOPT, 0xFFFFFFFF);
	GRRLIB_PrintfTTF(x + GRRLIB_WidthTTF(myFont, label, FONTOPT)
	                   + GRRLIB_WidthTTF(myFont, " ", FONTOPT),
	                 y, myFont, pct, FONTOPT, 0xADFF2FFF);
}

//In-game controls
static void readevents(){
		
		WPAD_ScanPads();
		PAD_ScanPads();

		if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME || PAD_ButtonsDown(0) & PAD_BUTTON_START) { emustatus = 2; sound_stop(); }
		
		if(joystick == 1)//Wiimote
		{
			if (WPAD_ButtonsDown(0) & WPAD_BUTTON_A) snd_regs[14] &= ~0x01;
			if (WPAD_ButtonsDown(0) & WPAD_BUTTON_B) snd_regs[14] &= ~0x02;
			if (WPAD_ButtonsDown(0) & WPAD_BUTTON_1) snd_regs[14] &= ~0x04;
			if (WPAD_ButtonsDown(0) & WPAD_BUTTON_2) snd_regs[14] &= ~0x08;
		
			if (WPAD_ButtonsUp(0) & WPAD_BUTTON_A) snd_regs[14] |= 0x01;
			if (WPAD_ButtonsUp(0) & WPAD_BUTTON_B) snd_regs[14] |= 0x02;
			if (WPAD_ButtonsUp(0) & WPAD_BUTTON_1) snd_regs[14] |= 0x04;
			if (WPAD_ButtonsUp(0) & WPAD_BUTTON_2) snd_regs[14] |= 0x08;
	
			if (WPAD_ButtonsDown(0) & WPAD_BUTTON_RIGHT || WPAD_ButtonsHeld(0) & WPAD_BUTTON_RIGHT) alg_jch1 = 0xff;
			else if (WPAD_ButtonsDown(0) & WPAD_BUTTON_LEFT || WPAD_ButtonsHeld(0) & WPAD_BUTTON_LEFT) alg_jch1 = 0x00;
			else alg_jch1 = 0x80;
		
			if (WPAD_ButtonsDown(0) & WPAD_BUTTON_DOWN || WPAD_ButtonsHeld(0) & WPAD_BUTTON_DOWN) alg_jch0 = 0xff;
			else if (WPAD_ButtonsDown(0) & WPAD_BUTTON_UP || WPAD_ButtonsHeld(0) & WPAD_BUTTON_UP) alg_jch0 = 0x00;
			else alg_jch0 = 0x80;
		}else{ //GC
			if (PAD_ButtonsDown(0) & PAD_BUTTON_X) snd_regs[14] &= ~0x01;
			if (PAD_ButtonsDown(0) & PAD_BUTTON_Y) snd_regs[14] &= ~0x02;
			if (PAD_ButtonsDown(0) & PAD_BUTTON_A) snd_regs[14] &= ~0x04;
			if (PAD_ButtonsDown(0) & PAD_BUTTON_B) snd_regs[14] &= ~0x08;
			
			if (PAD_ButtonsUp(0) & PAD_BUTTON_X) snd_regs[14] |= 0x01;
			if (PAD_ButtonsUp(0) & PAD_BUTTON_Y) snd_regs[14] |= 0x02;
			if (PAD_ButtonsUp(0) & PAD_BUTTON_A) snd_regs[14] |= 0x04;
			if (PAD_ButtonsUp(0) & PAD_BUTTON_B) snd_regs[14] |= 0x08;



			if (PAD_ButtonsDown(0) & PAD_BUTTON_UP || PAD_ButtonsHeld(0) & PAD_BUTTON_UP) alg_jch1 = 0xff;
			else if (PAD_ButtonsDown(0) & PAD_BUTTON_DOWN || PAD_ButtonsHeld(0) & PAD_BUTTON_DOWN) alg_jch1 = 0x00;
			else alg_jch1 = (PAD_StickY(0)) + 0x80;

			if (PAD_ButtonsDown(0) & PAD_BUTTON_RIGHT || PAD_ButtonsHeld(0) & PAD_BUTTON_RIGHT) alg_jch0 = 0xff;
			else if (PAD_ButtonsDown(0) & PAD_BUTTON_LEFT || PAD_ButtonsHeld(0) & PAD_BUTTON_LEFT) alg_jch0 = 0x00;
			else alg_jch0 = (PAD_StickX(0)) + 0x80;
		}		
}

//Checks the file extension - no further checks are needed
bool isRom(char* string)
{
	if(string[strlen(string)-3] == 'V' || string[strlen(string)-3] == 'v')
		if(string[strlen(string)-2] == 'E' || string[strlen(string)-2] == 'e')
			if(string[strlen(string)-1] == 'C' || string[strlen(string)-1] == 'c')
				return 1;
	return 0;
}

//Cartridge categories cycled in the menu. NA (built-in Mine Storm) is the
//default; Official/Mods/Prototypes are bundled into the .dol; SD is the
//user's card (SD or USB folded together, browsed like before).
typedef enum { CAT_NA = 0, CAT_OFFICIAL, CAT_MODS, CAT_PROTOTYPES, CAT_DEMOS, CAT_SD } Category;
#define CAT_COUNT 6

/* Cartridge selection remembered across a "Turn vectrex OFF": re-entering the
 * title menu restores the last category and browse index, to avoid scrolling
 * back through the same games every time. Saved when a game is chosen,
 * restored when Menu() rebuilds. */
static Category savedCategory = CAT_NA;
static s16 savedBrowse = 0;

static const CatalogEntry* categoryTable(Category cat){
	switch(cat){
		case CAT_OFFICIAL:   return catalog_official;
		case CAT_MODS:       return catalog_mods;
		case CAT_PROTOTYPES: return catalog_prototypes;
		case CAT_DEMOS:      return catalog_demos;
		default:             return NULL;
	}
}
static int categoryEmbeddedCount(Category cat){
	switch(cat){
		case CAT_OFFICIAL:   return catalog_official_count;
		case CAT_MODS:       return catalog_mods_count;
		case CAT_PROTOTYPES: return catalog_prototypes_count;
		case CAT_DEMOS:      return catalog_demos_count;
		default:             return 0;
	}
}
static const char* categoryLabel(Category cat, u8 sdDevice){
	switch(cat){
		case CAT_OFFICIAL:   return "[Official]";
		case CAT_MODS:       return "[Mods]";
		case CAT_PROTOTYPES: return "[Prototypes]";
		case CAT_DEMOS:      return "[Demos]";
		case CAT_SD:         return (sdDevice == 1) ? "[USB]" : "[SD]";
		default:             return "[NA]";
	}
}

/* Credits roll, reached from the title screen's "Credits" row. The names come
 * from the credits_lines[] table (see credits.c). It scrolls bottom-to-top once
 * and then returns to the title menu (or sooner, on 1 or B). A line ending in
 * ':' is a section header (title green), any other non-empty line is a name
 * (white), "" is a spacer. */
static void CreditsScroll(){
	const int LINEH = 34;   //vertical stride between successive lines
	const int SIZE  = 28;   //name font size
	const int HSIZE = 32;   //section-header font size
	const int SCRH  = 480;  //framebuffer height (SCR_CY * 2)
	int total = credits_count * LINEH;
	int cx[credits_count];       //centred x per line   -- the lines never change,
	unsigned sz[credits_count];  //font size per line      so measure them once here
	u32 col[credits_count];      //colour per line         rather than every frame
	f32 y = SCRH;                //first line starts just below the screen
	int done = 0, i;

	for(i = 0; i < credits_count; i++){
		const char *ln = credits_lines[i];
		int n = (int) strlen(ln);
		int hdr = (n > 0 && ln[n-1] == ':'); //section header: "Demo creators:" etc.
		sz[i]  = hdr ? HSIZE : SIZE;
		col[i] = hdr ? 0x228B22FF : 0xFFFFFFFF; //title green for headers, white for names
		cx[i]  = (n > 0) ? centerXTTF(ln, sz[i]) : 0;
	}

	while(!done){
		WPAD_ScanPads();
		PAD_ScanPads();
		if((WPAD_ButtonsDown(0) & (WPAD_BUTTON_1 | WPAD_BUTTON_B)) ||
		   (PAD_ButtonsDown(0) & PAD_BUTTON_B))
			done = 1;

		GRRLIB_FillScreen(0x000000FF);
		for(i = 0; i < credits_count; i++){
			int ly = (int)y + i * LINEH;
			if(credits_lines[i][0] == '\0') continue; //blank spacer
			if(ly <= -LINEH || ly >= SCRH) continue;  //off-screen: skip the raster
			GRRLIB_PrintfTTF(cx[i], ly, myFont, credits_lines[i], sz[i], col[i]);
		}
		GRRLIB_Render();

		y -= 1.0f;                        //scroll speed, pixels per frame
		if(y + total < 0) done = 1;       //last line has cleared the top: back to the title
	}
}

void PauseMenu(); //also serves the title screen's Settings entry (see settingsFromTitle)

/* Release Menu()'s scratch state. The rom-location tables are sized by the
 * number of ROMs on the card and the original code never freed them, so every
 * trip through the menu leaked a block proportional to that count -- which is
 * exactly the 2012 release notes' "crashes after loading a certain number of
 * ROMs, and more ROMs on the card means fewer loads before it crashes". The
 * leak also fragments the heap, and each game load needs ~750KB *contiguous*
 * for its overlay texture, so it bites long before the arena is really full.
 * Idempotent: safe to call on any exit path and again at the end. */
static void menuRelease(DIR **pdirSD, DIR **pdirUSB, u16 **romlocSD, u16 **romlocUSB){
	if(*pdirSD != NULL)  { closedir(*pdirSD);  *pdirSD = NULL; }
	if(*pdirUSB != NULL) { closedir(*pdirUSB); *pdirUSB = NULL; }
	free(*romlocSD);  *romlocSD = NULL;
	free(*romlocUSB); *romlocUSB = NULL;
}

//Main menu.
void Menu()
{
	s16 i=0, roms[2]={0, 0};
	u8 MenuOption = 1, sdDevice = 0;
	Category currentCategory = savedCategory; //restore the last cartridge selection (see savedCategory/savedBrowse)
	u16 * romlocSD = NULL, * romlocUSB = NULL;
	int turnOn=0;

	/* Text metrics for the title screen. They depend on the font sizes, which
	 * scale with the Screen Size setting, so they are (re)computed inside the
	 * loop whenever optScreenSize changes rather than once up front. */
	int xOn = 0, xSet = 0, xExit = 0, xCred = 0, wCart = 0, wCur = 0;
	unsigned szHead = FONTHEAD, szOpt = FONTOPT, szCur = (FONTHEAD+FONTOPT+FONTROM)/3;
	int lastScreenSize = -1;
	f32 s;

	DIR * pdirSD, * pdirUSB;
	struct dirent * pent = NULL;
	pdirSD=opendir("sd:/vec");
	pdirUSB=opendir("usb:/vec");

	if (pdirSD != NULL)
		while ((pent=readdir(pdirSD))!=NULL) //Count the roms (SD)
			if (isRom(pent->d_name)) roms[0]++;

	if (pdirUSB != NULL)
		while ((pent=readdir(pdirUSB))!=NULL) //Count the roms (USB)
			if (isRom(pent->d_name)) roms[1]++;

	if (roms[0] > 0) //SD
	{
		romlocSD = (u16 *)malloc(sizeof(u16)*roms[0]); //Allocate memory to store roms' "locations"
		rewinddir(pdirSD);
		while ((pent=readdir(pdirSD))!=NULL) //Store all the locations
			if (isRom(pent->d_name))
				romlocSD[i++] = telldir(pdirSD)-1; //The -1 is required here; the reason is still unclear
		i=0;
	}

	if (roms[1] > 0) //USB
	{
		romlocUSB = (u16 *)malloc(sizeof(u16)*roms[1]);
		rewinddir(pdirUSB);
		while ((pent=readdir(pdirUSB))!=NULL)
			if (isRom(pent->d_name))
				romlocUSB[i++] = telldir(pdirUSB)-1;
		i=0;
	}

	pent = NULL;
	emustatus = 1;
	i = savedBrowse; //restore the last browse index now the scan has finished using i as scratch

	//Title-screen Y scaled toward the screen centre by the Screen Size setting.
	#define TSY(Y) ((int)(SCR_CY + ((f32)(Y) - SCR_CY) * s))

	while(!turnOn)
	{
		int count; //number of ROMs browsable in the current category

		//The options screen owns the whole frame while it's open
		if(settingsFromTitle){ PauseMenu(); continue; }

		WPAD_ScanPads();
		PAD_ScanPads();

		//Browse ROMs within the current category (Wiimote U/D, GC L/R)
		if (WPAD_ButtonsDown(0) & WPAD_BUTTON_UP || PAD_ButtonsDown(0) & PAD_BUTTON_LEFT) i--;
		if (WPAD_ButtonsDown(0) & WPAD_BUTTON_DOWN || PAD_ButtonsDown(0) & PAD_BUTTON_RIGHT) i++;

		//Move the menu cursor (Wiimote R/L, GC U/D)
		if (WPAD_ButtonsDown(0) & WPAD_BUTTON_RIGHT || PAD_ButtonsDown(0) & PAD_BUTTON_UP) MenuOption--;
		if (WPAD_ButtonsDown(0) & WPAD_BUTTON_LEFT || PAD_ButtonsDown(0) & PAD_BUTTON_DOWN) MenuOption++;
		if(MenuOption < 1) MenuOption = 5; if(MenuOption > 5) MenuOption = 1;

		if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME || PAD_ButtonsDown(0) & PAD_BUTTON_START)
		{
			if(MenuOption == 4)
			{
				menuRelease(&pdirSD, &pdirUSB, &romlocSD, &romlocUSB);
				GRRLIB_Exit(); GRRLIB_FreeTexture(splash);
				exit(0);
			}else
				MenuOption = 4;

		}

		if (WPAD_ButtonsDown(0) & ~0x00000F80 || PAD_ButtonsDown(0) & ~0x100F) //Exclude PADs and HOME/START. Any other button triggers the selected option
		{
			switch(MenuOption)
			{
				case 2:	//Cycle cartridge category: Wiimote 1/-/B = back, 2/+/A = forward; GC L = back, R = forward
				{
					u32 wd = WPAD_ButtonsDown(0), pd = PAD_ButtonsDown(0);
					int back    = (wd & (WPAD_BUTTON_1 | WPAD_BUTTON_MINUS | WPAD_BUTTON_B)) || (pd & PAD_TRIGGER_L);
					int forward = (wd & (WPAD_BUTTON_2 | WPAD_BUTTON_PLUS  | WPAD_BUTTON_A)) || (pd & PAD_TRIGGER_R);
					if(back)         { currentCategory = (Category)((currentCategory + CAT_COUNT - 1) % CAT_COUNT); i = 0; }
					else if(forward) { currentCategory = (Category)((currentCategory + 1) % CAT_COUNT); i = 0; }
				}
				break;
				case 3: //Settings (shares the pause menu's option pages)
					settingsFromTitle = 1; previewLoad();
					pauseMenu[0] = 0; pauseMenu[1] = 2; //option list, first row
				break;
				case 4: //Exit!
					menuRelease(&pdirSD, &pdirUSB, &romlocSD, &romlocUSB);
					GRRLIB_Exit(); GRRLIB_FreeTexture(splash);
					exit(0);
				break;
				case 5: //Credits roll (1/B returns here)
					CreditsScroll();
				break;
				default:
				{
					turnOn = 1;
					savedCategory = currentCategory; savedBrowse = i; //remember the position for next time
					if (WPAD_ButtonsDown(0) & ~0x00000F80) joystick = 1;
					else joystick = 0;

					if (currentCategory == CAT_NA)
					{
						menuRelease(&pdirSD, &pdirUSB, &romlocSD, &romlocUSB);
						overlay = GRRLIB_LoadTexture(Minestorm_png); //cart[] stays empty: Mine Storm runs from rom[]
					}
					else if (currentCategory == CAT_SD)
					{
						if (pent != NULL)
						{
							FILE *cartfile;
							char cartname[300], overlaypath[300]; //"<dev>:/vec/" + a full-length 255-char d_name
							const char *dev = (sdDevice == 1) ? "usb" : "sd";

							snprintf(cartname, sizeof(cartname), "%s:/vec/%s", dev, pent->d_name);
							snprintf(overlaypath, sizeof(overlaypath), "%s:/vec/%s", dev, pent->d_name);
							overlaypath[strlen(overlaypath)-3] = 'p'; overlaypath[strlen(overlaypath)-2] = 'n';	overlaypath[strlen(overlaypath)-1] = 'g';

							cartfile = fopen (cartname, "rb");
							if (cartfile != NULL) {
								memset(cart, 0, sizeof(cart));
								fread (cart, 1, sizeof (cart), cartfile);
								fclose (cartfile);
								menuRelease(&pdirSD, &pdirUSB, &romlocSD, &romlocUSB);
								overlay = GRRLIB_LoadTextureFromFile(overlaypath);

								if(overlay == NULL){ //Try uppercase extension...
									overlaypath[strlen(overlaypath)-3] = 'P'; overlaypath[strlen(overlaypath)-2] = 'N';	overlaypath[strlen(overlaypath)-1] = 'G';
									overlay = GRRLIB_LoadTextureFromFile(overlaypath);
								}

							} else { //ERROR: Freak out! TODO: Display an error msg
								turnOn = 0; //Turn the vectrex Off
							}
						} else {
							turnOn = 0; //Nothing on SD/USB to load
						}
					}
					else //Bundled category: Official / Mods / Prototypes / Demos
					{
						const CatalogEntry *tbl = categoryTable(currentCategory);
						int cnt = categoryEmbeddedCount(currentCategory);
						if (i < 0 || i >= cnt) i = 0;
						menuRelease(&pdirSD, &pdirUSB, &romlocSD, &romlocUSB);
						memset(cart, 0, sizeof(cart));
						memcpy(cart, tbl[i].rom, tbl[i].rom_size);
						//Demos never had overlays; a NULL here just means "no overlay"
						overlay = (tbl[i].overlay != NULL) ? GRRLIB_LoadTexture(tbl[i].overlay) : NULL;
					}
				}
				break;
			}
		}

		//Clamp the browse index to the current category's ROM count
		if(currentCategory == CAT_NA) count = 1;
		else if(currentCategory == CAT_SD) count = roms[0] + roms[1];
		else count = categoryEmbeddedCount(currentCategory);
		if(count < 1) count = 1;
		if(i < 0) i = count - 1; if(i >= count) i = 0;

		//Resolve the SD/USB dirent for the current combined browse index.
		//The handle/table checks matter because menuRelease() clears them the
		//moment a game is chosen; no path reaches here afterwards today, but a
		//stale seekdir would be a silent crash rather than a visible bug.
		if(currentCategory == CAT_SD && (roms[0] + roms[1]) > 0){
			if(i < roms[0]) { if(pdirSD  != NULL && romlocSD  != NULL) { sdDevice = 0; seekdir(pdirSD,  romlocSD[i]);            pent = readdir(pdirSD);  } }
			else            { if(pdirUSB != NULL && romlocUSB != NULL) { sdDevice = 1; seekdir(pdirUSB, romlocUSB[i - roms[0]]); pent = readdir(pdirUSB); } }
		}

		//The whole title screen shrinks with the Screen Size setting, exactly like
		//the in-game view, so overscan can't clip it. Text positions and font sizes
		//are remeasured only when the setting actually changes, not every frame.
		s = optScreenSize / 255.0f;
		if((int)optScreenSize != lastScreenSize){
			lastScreenSize = optScreenSize;
			szHead = (unsigned)(FONTHEAD * s + 0.5f);
			szOpt  = (unsigned)(FONTOPT  * s + 0.5f);
			szCur  = (unsigned)(((FONTHEAD+FONTOPT+FONTROM)/3) * s + 0.5f);
			xOn    = centerXTTF("Turn vectrex ON", szHead);
			xSet   = centerXTTF("Settings", szHead);
			xExit  = centerXTTF("Return to loader", szHead);
			xCred  = centerXTTF("Credits", szHead);
			wCart  = GRRLIB_WidthTTF(myFont, "Cartridge ", szOpt);
			wCur   = GRRLIB_WidthTTF(myFont, ">", szCur);
		}

		GRRLIB_FillScreen(0x000000FF); //black border behind the splash when it's shrunk below 100%
		GRRLIB_DrawImg(SCR_CX * (1.0f - s), SCR_CY * (1.0f - s), splash, 0, s, s, 0xFFFFFFFF);

		/* "Cartridge [Category]" is centred as one string. Centring only the
		 * word and hanging the label off its right edge threw the row off to
		 * the right, by an amount that changed with the label -- [Prototypes]
		 * is nearly three times the width of [NA]. */
		{
			const char *catlabel = categoryLabel(currentCategory, sdDevice);
			int xCart = (rmode->fbWidth - (wCart + (int) GRRLIB_WidthTTF(myFont, catlabel, szOpt))) / 2;

			GRRLIB_PrintfTTF(xOn,   TSY(178), myFont, "Turn vectrex ON",  szHead, 0x228B22FF);
			GRRLIB_PrintfTTF(xCart, TSY(216), myFont, "Cartridge",        szOpt,  0xFFFFFFFF);
			GRRLIB_PrintfTTF(xCart + wCart, TSY(216), myFont, catlabel,   szOpt,  0xFF0000FF);
			GRRLIB_PrintfTTF(xSet,  TSY(251), myFont, "Settings",         szHead, 0xFFFFFFFF);
			GRRLIB_PrintfTTF(xExit, TSY(290), myFont, "Return to loader", szHead, 0xFFFFFFFF);
			GRRLIB_PrintfTTF(xCred, TSY(420), myFont, "Credits",          szHead, 0x228B22FF);

			if(MenuOption == 1)      GRRLIB_PrintfTTF(xOn   - wCur, TSY(178), myFont, ">", szCur, 0xFF0000FF);
			else if(MenuOption == 2) GRRLIB_PrintfTTF(xCart - wCur, TSY(216), myFont, ">", szCur, 0xFF0000FF);
			else if(MenuOption == 3) GRRLIB_PrintfTTF(xSet  - wCur, TSY(251), myFont, ">", szCur, 0xFF0000FF);
			else if(MenuOption == 4) GRRLIB_PrintfTTF(xExit - wCur, TSY(290), myFont, ">", szCur, 0xFF0000FF);
			else                     GRRLIB_PrintfTTF(xCred - wCur, TSY(420), myFont, ">", szCur, 0xFF0000FF);
		}

		if(currentCategory == CAT_NA)
			PrintRomTitle(TSY(340), "Minestorm", s);
		else if(currentCategory == CAT_SD){
			if(pent != NULL){
				char title[256];
				int L;
				strncpy(title, pent->d_name, sizeof(title)-1); title[sizeof(title)-1] = '\0';
				L = strlen(title); if(L > 4) title[L-4] = '\0'; //Drop the .vec extension for display
				PrintRomTitle(TSY(340), title, s);
			} else
				PrintRomTitle(TSY(340), "No ROMs on SD/USB", s);
		} else
			PrintRomTitle(TSY(340), categoryTable(currentCategory)[i].title, s);

		GRRLIB_Render();
	}
	#undef TSY

	menuRelease(&pdirSD, &pdirUSB, &romlocSD, &romlocUSB); //belt and braces: no path leaves the scratch state behind
}

void PauseMenu()
{
	u8 input = 0;
	int MenuOffsets[5] = {	(rmode->fbWidth-GRRLIB_WidthTTF(myFont,"Resume Game", FONTHEAD))/2,
							(rmode->fbWidth-GRRLIB_WidthTTF(myFont,"Turn vectrex OFF", FONTHEAD))/2,
							(rmode->fbWidth-GRRLIB_WidthTTF(myFont,"Custom Color [ON]", FONTOPT))/2,
							GRRLIB_WidthTTF(myFont,"Custom Color ", FONTOPT),
							GRRLIB_WidthTTF(myFont,">", (FONTHEAD+FONTOPT)/2)};
	
	WPAD_ScanPads();
	PAD_ScanPads();
	
	if (WPAD_ButtonsDown(0) & WPAD_BUTTON_RIGHT || PAD_ButtonsDown(0) & PAD_BUTTON_UP) input = 1;
	if (WPAD_ButtonsDown(0) & WPAD_BUTTON_LEFT || PAD_ButtonsDown(0) & PAD_BUTTON_DOWN) input = 2;
	if (WPAD_ButtonsDown(0) & WPAD_BUTTON_UP || PAD_ButtonsDown(0) & PAD_BUTTON_LEFT) input = 3;
	if (WPAD_ButtonsDown(0) & WPAD_BUTTON_DOWN || PAD_ButtonsDown(0) & PAD_BUTTON_RIGHT) input = 4;
	if (WPAD_ButtonsDown(0) & ~0x00000F80 || PAD_ButtonsDown(0) & ~0x100F) input = 5;
	if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME || PAD_ButtonsDown(0) & PAD_BUTTON_START) input = 6;

	if(settingsFromTitle) preview_render(); //no game to freeze yet: preview the options on a still instead
	else pause_render(); //Render the screen as it was when the emulation was interrumped

	//Holding A/R peeks at the paused game behind the menu; pointless on the title screen
	if(settingsFromTitle || (!(PAD_ButtonsHeld(0) & PAD_TRIGGER_R) && !(WPAD_ButtonsHeld(0) & WPAD_BUTTON_A)))
	{
		GRRLIB_Rectangle(0, 160, rmode->fbWidth, 160, 0x000000D0, 1);

		switch(pauseMenu[0]){
			case 2: //Overlay
			//Process input
				if(input == 3) optOverlay[1] -= (optOverlay[1] == 0 ? 0 : 15);
				if(input == 4) optOverlay[1] += (optOverlay[1] == 255 ? 0 : 15);
				if(input == 5 || input == 6) {pauseMenu[1] = pauseMenu[0] + 1; pauseMenu[0] = 0;} //cursor lands back on this setting's own row
			
			//Draw
				PrintCenteredTTF(155, "Overlay", FONTHEAD, 0x228B22FF);
				drawSliderCaption(215, "Opacity", optOverlay[1]);
				GRRLIB_PrintfTTF( (rmode->fbWidth-GRRLIB_WidthTTF(myFont, "[                  ]", FONTOPT))/2, 240, myFont, "[                  ]", FONTOPT, 0xFFFFFFFF);
				GRRLIB_Rectangle( (rmode->fbWidth-GRRLIB_WidthTTF(myFont, "[                  ]", FONTOPT))/2 + GRRLIB_WidthTTF(myFont, "[", FONTOPT) , 253, (GRRLIB_WidthTTF(myFont, "                  ", FONTOPT) - 6) * optOverlay[1] / 255 , 7, 0xFFFFFFFF, 1);
			break;
			case 3: //CustomColor
			//Process input
				if(input == 1) pauseMenu[1]--;	if(input == 2) pauseMenu[1]++;
				if(pauseMenu[1] < 1) pauseMenu[1] = 3; if(pauseMenu[1] > 3) pauseMenu[1] = 1;
				
				if(input == 3) optVtxCustomColor[pauseMenu[1]] -= (optVtxCustomColor[pauseMenu[1]] == 0 ? 0 : 15);
				if(input == 4) optVtxCustomColor[pauseMenu[1]] += (optVtxCustomColor[pauseMenu[1]] == 255 ? 0 : 15);
				if(input == 5 || input == 6) {pauseMenu[1] = pauseMenu[0] + 1; pauseMenu[0] = 0;} //cursor lands back on this setting's own row
			
			//Draw
				PrintCenteredTTF(155, "Custom Color", FONTHEAD, 0x228B22FF);
				PrintCenteredTTF(195, "RGB Components", FONTOPT, 0xFFFFFFFF);
				GRRLIB_PrintfTTF( (rmode->fbWidth-GRRLIB_WidthTTF(myFont, "[                  ]", FONTOPT))/2, 220, myFont, "[                  ]", FONTOPT, 0xFF0000FF);
				GRRLIB_Rectangle( (rmode->fbWidth-GRRLIB_WidthTTF(myFont, "[                  ]", FONTOPT))/2 + GRRLIB_WidthTTF(myFont, "[", FONTOPT) , 233, (GRRLIB_WidthTTF(myFont, "                  ", FONTOPT) - 6) * optVtxCustomColor[1] / 255 , 7, 0xFF0000FF, 1);
				GRRLIB_PrintfTTF( (rmode->fbWidth-GRRLIB_WidthTTF(myFont, "[                  ]", FONTOPT))/2, 240, myFont, "[                  ]", FONTOPT, 0x00FF00FF);
				GRRLIB_Rectangle( (rmode->fbWidth-GRRLIB_WidthTTF(myFont, "[                  ]", FONTOPT))/2 + GRRLIB_WidthTTF(myFont, "[", FONTOPT) , 253, (GRRLIB_WidthTTF(myFont, "                  ", FONTOPT) - 6) * optVtxCustomColor[2] / 255 , 7, 0x00FF00FF, 1);
				GRRLIB_PrintfTTF( (rmode->fbWidth-GRRLIB_WidthTTF(myFont, "[                  ]", FONTOPT))/2, 260, myFont, "[                  ]", FONTOPT, 0x0000FFFF);
				GRRLIB_Rectangle( (rmode->fbWidth-GRRLIB_WidthTTF(myFont, "[                  ]", FONTOPT))/2 + GRRLIB_WidthTTF(myFont, "[", FONTOPT) , 273, (GRRLIB_WidthTTF(myFont, "                  ", FONTOPT) - 6) * optVtxCustomColor[3] / 255 , 7, 0x0000FFFF, 1);
				
				if(pauseMenu[1] == 1) GRRLIB_PrintfTTF((rmode->fbWidth-GRRLIB_WidthTTF(myFont, "[                  ]", FONTOPT))/2-MenuOffsets[4], 220, myFont,">", (FONTHEAD+FONTOPT)/2,0xFF0000FF);
				else if(pauseMenu[1] == 2) GRRLIB_PrintfTTF((rmode->fbWidth-GRRLIB_WidthTTF(myFont, "[                  ]", FONTOPT))/2-MenuOffsets[4], 240, myFont,">", (FONTHEAD+FONTOPT)/2,0x00FF00FF);
				else GRRLIB_PrintfTTF((rmode->fbWidth-GRRLIB_WidthTTF(myFont, "[                  ]", FONTOPT))/2-MenuOffsets[4], 260, myFont,">", (FONTHEAD+FONTOPT)/2,0x0000FFFF);
			break;
			case 4: //Glow
			//Process input
				if(input == 1) pauseMenu[1]--;	if(input == 2) pauseMenu[1]++;
				if(pauseMenu[1] < 1) pauseMenu[1] = 2; if(pauseMenu[1] > 2) pauseMenu[1] = 1;

				if(input == 5 || input == 6) {pauseMenu[1] = pauseMenu[0] + 1; pauseMenu[0] = 0;} //cursor lands back on this setting's own row

				if(input == 3)
					switch(pauseMenu[1])
					{
						case 1: optGlow[1] -= (optGlow[1] == 1 ? 0 : 1); break;
						case 2: optGlow[2] -= (optGlow[2] == 0 ? 0 : 15); break;
					}
				if(input == 4)
					switch(pauseMenu[1])
					{
						case 1: optGlow[1] += (optGlow[1] == 35 ? 0 : 1); break;
						case 2: optGlow[2] += (optGlow[2] == 255 ? 0 : 15); break;
					}

			//Draw
				PrintCenteredTTF(155, "Glow", FONTHEAD, 0x228B22FF);
				GRRLIB_PrintfTTF( (rmode->fbWidth-GRRLIB_WidthTTF(myFont,"Factor 15", FONTOPT))/2, 200, myFont,"Factor", FONTOPT, 0xFFFFFFFF);
				{
					char factor[8];
					sprintf(factor, "%d", optGlow[1]);
					GRRLIB_PrintfTTF( (rmode->fbWidth-GRRLIB_WidthTTF(myFont,"Factor 15", FONTOPT))/2 + GRRLIB_WidthTTF(myFont,"Factor ", FONTOPT), 200, myFont, factor, FONTOPT, 0xFF0000FF);
				}

				drawSliderCaption(227, "Opacity", optGlow[2]);
				GRRLIB_PrintfTTF( (rmode->fbWidth-GRRLIB_WidthTTF(myFont, "[                  ]", FONTOPT))/2, 247, myFont, "[                  ]", FONTOPT, 0xFFFFFFFF);
				GRRLIB_Rectangle( (rmode->fbWidth-GRRLIB_WidthTTF(myFont, "[                  ]", FONTOPT))/2 + GRRLIB_WidthTTF(myFont, "[", FONTOPT) , 260, (GRRLIB_WidthTTF(myFont, "                  ", FONTOPT) - 6) * optGlow[2] / 255 , 7, 0xFFFFFFFF, 1);

				if(pauseMenu[1] == 2) GRRLIB_PrintfTTF((rmode->fbWidth-GRRLIB_WidthTTF(myFont, "[                  ]", FONTOPT))/2-MenuOffsets[4], 247, myFont,">", (FONTHEAD+FONTOPT)/2,0xADFF2FFF);
				else GRRLIB_PrintfTTF((rmode->fbWidth-GRRLIB_WidthTTF(myFont,"Factor 15", FONTOPT))/2-MenuOffsets[4], 200, myFont,">", (FONTHEAD+FONTOPT)/2,0xADFF2FFF);
			break;
			case 5: //Persistence
			//Process input
				if(input == 1) pauseMenu[1]--;	if(input == 2) pauseMenu[1]++;
				if(pauseMenu[1] < 1) pauseMenu[1] = 3; if(pauseMenu[1] > 3) pauseMenu[1] = 1;
				
				if(input == 5 || input == 6){ pauseMenu[1] = pauseMenu[0] + 1; pauseMenu[0] = 0;} //cursor lands back on this setting's own row
				if(input == 3)
					switch(pauseMenu[1])
					{
						case 1: optPersistence[1] -= (optPersistence[1] == 1 ? 0 : 1); break;
						case 2: optPersistence[2] -= (optPersistence[2] == 0 ? 0 : 15); break;
						case 3: optPersistence[3] -= (optPersistence[3] == 0 ? 0 : 15); break;
					}

				if(input == 4)
					switch(pauseMenu[1])
					{
						case 1: optPersistence[1] += (optPersistence[1] == 20 ? 0 : 1); break;
						case 2: optPersistence[2] += (optPersistence[2] == 255 ? 0 : 15); break;
						case 3: optPersistence[3] += (optPersistence[3] == 255 ? 0 : 15); break;
					}
			
			//Draw
				PrintCenteredTTF(155, "Persistence", FONTHEAD, 0x228B22FF);
				GRRLIB_PrintfTTF( (rmode->fbWidth-GRRLIB_WidthTTF(myFont,"Frames 15", FONTOPT))/2, 200, myFont,"Frames", FONTOPT, 0xFFFFFFFF);
				{
					char frames[8];
					sprintf(frames, "%d", optPersistence[1]);
					GRRLIB_PrintfTTF( (rmode->fbWidth-GRRLIB_WidthTTF(myFont,"Frames 15", FONTOPT))/2 + GRRLIB_WidthTTF(myFont,"Frames ", FONTOPT), 200, myFont, frames, FONTOPT, 0xFF0000FF);
				}
				drawSliderCaption(227, "Grayscale", optPersistence[2]);
				GRRLIB_PrintfTTF( (rmode->fbWidth-GRRLIB_WidthTTF(myFont, "[                  ]", FONTOPT))/2, 247, myFont, "[                  ]", FONTOPT, RGBA(optPersistence[2], optPersistence[2], optPersistence[2], 0xFF));
				GRRLIB_Rectangle( (rmode->fbWidth-GRRLIB_WidthTTF(myFont, "[                  ]", FONTOPT))/2 + GRRLIB_WidthTTF(myFont, "[", FONTOPT) , 260, (GRRLIB_WidthTTF(myFont, "                  ", FONTOPT) - 6) * optPersistence[2] / 255 , 7, RGBA(optPersistence[2], optPersistence[2], optPersistence[2], 0xFF), 1);

				drawSliderCaption(265, "Opacity", optPersistence[3]);
				GRRLIB_PrintfTTF( (rmode->fbWidth-GRRLIB_WidthTTF(myFont, "[                  ]", FONTOPT))/2, 285, myFont, "[                  ]", FONTOPT, 0xFFFFFFFF);
				GRRLIB_Rectangle( (rmode->fbWidth-GRRLIB_WidthTTF(myFont, "[                  ]", FONTOPT))/2 + GRRLIB_WidthTTF(myFont, "[", FONTOPT) , 298, (GRRLIB_WidthTTF(myFont, "                  ", FONTOPT) - 6) * optPersistence[3] / 255 , 7, 0xFFFFFFFF, 1);
				
				if(pauseMenu[1] == 2) GRRLIB_PrintfTTF((rmode->fbWidth-GRRLIB_WidthTTF(myFont, "[                  ]", FONTOPT))/2-MenuOffsets[4], 247, myFont,">", (FONTHEAD+FONTOPT)/2,0xADFF2FFF);
				else if(pauseMenu[1] == 3) GRRLIB_PrintfTTF((rmode->fbWidth-GRRLIB_WidthTTF(myFont, "[                  ]", FONTOPT))/2-MenuOffsets[4], 285, myFont,">", (FONTHEAD+FONTOPT)/2,0xADFF2FFF);
				else GRRLIB_PrintfTTF((rmode->fbWidth-GRRLIB_WidthTTF(myFont,"Frames 15", FONTOPT))/2-MenuOffsets[4], 200, myFont,">", (FONTHEAD+FONTOPT)/2,0xADFF2FFF);
			break;
			case 6: //Screen Size (overscan correction)
			//Process input
				if(input == 3) optScreenSize -= (optScreenSize <= 150 ? 0 : 15);
				if(input == 4) optScreenSize += (optScreenSize >= 255 ? 0 : 15);
				if(input == 5 || input == 6) {pauseMenu[1] = pauseMenu[0] + 1; pauseMenu[0] = 0;} //cursor lands back on this setting's own row

			//Draw
				PrintCenteredTTF(155, "Screen Size", FONTHEAD, 0x228B22FF);
				drawSliderCaption(215, "Size", optScreenSize);
				GRRLIB_PrintfTTF( (rmode->fbWidth-GRRLIB_WidthTTF(myFont, "[                  ]", FONTOPT))/2, 240, myFont, "[                  ]", FONTOPT, 0xFFFFFFFF);
				GRRLIB_Rectangle( (rmode->fbWidth-GRRLIB_WidthTTF(myFont, "[                  ]", FONTOPT))/2 + GRRLIB_WidthTTF(myFont, "[", FONTOPT) , 253, (GRRLIB_WidthTTF(myFont, "                  ", FONTOPT) - 6) * optScreenSize / 255 , 7, 0xFFFFFFFF, 1);
			break;
			default: //Main pause menu

			//Input processing
				if (input == 1) pauseMenu[1]--;	if (input == 2) pauseMenu[1]++;
				{
					/* the title-screen version has no "Resume Game" row, so it
					 * starts at 2. only wrap on an actual up-press -- landing
					 * here at 1 after closing a sub-page must clamp, not jump.
					 */
					u8 lo = settingsFromTitle ? 2 : 1;
					if(pauseMenu[1] < lo) pauseMenu[1] = (input == 1) ? 8 : lo;
					if(pauseMenu[1] > 8) pauseMenu[1] = lo;
				}

				if(input == 3 || input == 4){
					switch(pauseMenu[1]){
						case 2: joystick ^= 1; break;
						case 3:	optOverlay[0] ^= 1; break;
						case 4: optVtxCustomColor[0] ^= 1; break;
						case 5:	optGlow[0] ^= 1; break;
						case 6: optPersistence[0] ^= 1; break;
					}
				}

				if(input == 5){
					switch(pauseMenu[1]){
						case 1: if(!settingsFromTitle) emustatus = 1; break;
						//case 2 (Joystick) is a plain toggle, it has no sub-page
						case 3:	pauseMenu[0] = 2; break;
						case 4: pauseMenu[0] = 3; break;
						case 5:	pauseMenu[0] = 4; break;
						case 6: pauseMenu[0] = 5; break;
						case 7: pauseMenu[0] = 6; break;
						case 8: if(settingsFromTitle) { settingsFromTitle = 0; previewFree(); } else emustatus = 0; break;
					}
					 pauseMenu[1] = 1; //sub-pages use this as their own cursor
				}

				if(input == 6){
					if(settingsFromTitle) { settingsFromTitle = 0; previewFree(); } //HOME backs out to the title menu
					else pauseMenu[1] = 1;
				}
				
			//Drawing
				if(settingsFromTitle) PrintCenteredTTF(155, "Settings", FONTHEAD, 0x228B22FF);
				else GRRLIB_PrintfTTF( MenuOffsets[0], 155, myFont,"Resume Game",FONTHEAD, 0x228B22FF);
				GRRLIB_PrintfTTF( MenuOffsets[2], 176, myFont,"Joystick", FONTOPT,0xFFFFFFFF);
				GRRLIB_PrintfTTF( MenuOffsets[2], 194, myFont,"Overlay", FONTOPT,0xFFFFFFFF);
				GRRLIB_PrintfTTF( MenuOffsets[2], 212, myFont,"Custom Color", FONTOPT,0xFFFFFFFF);
				GRRLIB_PrintfTTF( MenuOffsets[2], 230, myFont,"Glow", FONTOPT,0xFFFFFFFF);
				GRRLIB_PrintfTTF( MenuOffsets[2], 248, myFont,"Persistence", FONTOPT,0xFFFFFFFF);
				GRRLIB_PrintfTTF( MenuOffsets[2], 266, myFont,"Screen Size", FONTOPT,0xFFFFFFFF);
				if(settingsFromTitle) PrintCenteredTTF(288, "Back", FONTHEAD, 0xFF0000FF);
				else GRRLIB_PrintfTTF( MenuOffsets[1], 288, myFont,"Turn vectrex OFF", FONTHEAD,0xFF0000FF);

				if (joystick == 1) GRRLIB_PrintfTTF( MenuOffsets[2]+MenuOffsets[3], 176, myFont,"[Wii]", FONTOPT, 0xADFF2FFF);
				else GRRLIB_PrintfTTF( MenuOffsets[2]+MenuOffsets[3], 176, myFont,"[GC]", FONTOPT, 0xADFF2FFF);

				if (optOverlay[0]) GRRLIB_PrintfTTF( MenuOffsets[2]+MenuOffsets[3], 194, myFont,"[ON]", FONTOPT, 0xADFF2FFF);
				else GRRLIB_PrintfTTF( MenuOffsets[2]+MenuOffsets[3],194, myFont,"[OFF]", FONTOPT, 0xADFF2FFF);

				if(optVtxCustomColor[0]) GRRLIB_PrintfTTF( MenuOffsets[2]+MenuOffsets[3], 212, myFont,"[ON]", FONTOPT, 0xADFF2FFF);
				else GRRLIB_PrintfTTF( MenuOffsets[2]+MenuOffsets[3], 212, myFont,"[OFF]", FONTOPT, 0xADFF2FFF);

				if (optGlow[0]) GRRLIB_PrintfTTF( MenuOffsets[2]+MenuOffsets[3], 230, myFont,"[ON]", FONTOPT, 0xADFF2FFF);
				else GRRLIB_PrintfTTF( MenuOffsets[2]+MenuOffsets[3], 230, myFont,"[OFF]", FONTOPT, 0xADFF2FFF);

				if (optPersistence[0]) GRRLIB_PrintfTTF( MenuOffsets[2]+MenuOffsets[3], 248, myFont,"[ON]", FONTOPT, 0xADFF2FFF);
				else GRRLIB_PrintfTTF( MenuOffsets[2]+MenuOffsets[3], 248, myFont,"[OFF]", FONTOPT, 0xADFF2FFF);

				{
					char sizebuf[8];
					sprintf(sizebuf, "[%d%%]", optScreenSize * 100 / 255);
					GRRLIB_PrintfTTF( MenuOffsets[2]+MenuOffsets[3], 266, myFont, sizebuf, FONTOPT, 0xADFF2FFF);
				}

				if(pauseMenu[1] == 1) { if(!settingsFromTitle) GRRLIB_PrintfTTF(MenuOffsets[0]-MenuOffsets[4], 155, myFont,">", (FONTHEAD+FONTOPT)/2,0xFF0000FF); }
				else if(pauseMenu[1] == 2) GRRLIB_PrintfTTF(MenuOffsets[2]-MenuOffsets[4], 176, myFont,">", (FONTHEAD+FONTOPT)/2,0xFF0000FF);
				else if(pauseMenu[1] == 3) GRRLIB_PrintfTTF(MenuOffsets[2]-MenuOffsets[4], 194, myFont,">", (FONTHEAD+FONTOPT)/2,0xFF0000FF);
				else if(pauseMenu[1] == 4) GRRLIB_PrintfTTF(MenuOffsets[2]-MenuOffsets[4], 212, myFont,">", (FONTHEAD+FONTOPT)/2,0xFF0000FF);
				else if(pauseMenu[1] == 5) GRRLIB_PrintfTTF(MenuOffsets[2]-MenuOffsets[4], 230, myFont,">", (FONTHEAD+FONTOPT)/2,0xFF0000FF);
				else if(pauseMenu[1] == 6) GRRLIB_PrintfTTF(MenuOffsets[2]-MenuOffsets[4], 248, myFont,">", (FONTHEAD+FONTOPT)/2,0xFF0000FF);
				else if(pauseMenu[1] == 7) GRRLIB_PrintfTTF(MenuOffsets[2]-MenuOffsets[4], 266, myFont,">", (FONTHEAD+FONTOPT)/2,0xFF0000FF);
				else GRRLIB_PrintfTTF(MenuOffsets[1]-MenuOffsets[4], 288, myFont,">", (FONTHEAD+FONTOPT)/2,0xFF0000FF);

			break;
		}

	}

	GRRLIB_Render();

	}

void osint_emuloop(){

	u64 next_ticks = gettime();
	vecx_reset();
	sound_stop(); //fresh audio state for the new game

	while(emustatus != 0) {
		if(emustatus == 1) { //Normal emulation
#ifndef DISABLE_SOUND
			/* Draw only when the audio ring can afford it. A slow draw (the
			 * blur/glow renderer on heavy scenes -- the original release notes
			 * already warned about its framerate -- and Dolphin generally)
			 * stalls the same loop that feeds audio. Audio wins: with a thin
			 * cushion the streak-breaker still needs a couple of chunks in the
			 * bank, and only a ~half-second absolute cap forces a draw
			 * unconditionally so the screen can never hard-freeze. */
			{
				static u32 skipped = 0;
				u32 backlog = snd_widx - snd_ridx;
				if(backlog > SND_CHUNK_SAMPLES * 4) { renderThisFrame = 1; skipped = 0; }
				else if(skipped >= 3 && backlog > SND_CHUNK_SAMPLES * 2) { renderThisFrame = 1; skipped = 0; }
				else if(skipped >= 25) { renderThisFrame = 1; skipped = 0; }
				else { renderThisFrame = 0; skipped++; }
			}
#endif
			vecx_emu((VECTREX_MHZ / 1000) * EMU_TIMER, 0);
			osint_playaudio();
			readevents();
		} else {
			PauseMenu();
		}

		{ /* pace the loop to EMU_TIMER using full-resolution timebase ticks.
		   * (the original code compared millisecond values but passed them
		   * to usleep as microseconds, sleeping 1000x too short -- the
		   * emulator always ran fast and bursty, which was also the old
		   * "fullspeed for 5 seconds after pausing" mystery bug.)
		   */
				u64 now = gettime();
				if(now < next_ticks)
					usleep(ticks_to_microsecs(next_ticks - now));
				else if(now - next_ticks > millisecs_to_ticks(200))
					next_ticks = now; //fell way behind; resync instead of sprinting
				next_ticks += millisecs_to_ticks(EMU_TIMER);
		}
	}
	
	//Vectrex turned off, get ready to load another game...
	sound_stop(); //silence the menu
	memset ( cart, 0, sizeof(cart)); //Empty the cartridge! to be able to play Minestorm if wanted
	if (overlay != NULL) { GRRLIB_FreeTexture(overlay); overlay = NULL; } //Free the texture containing the overlay

	//Persistence clean up. Clearing the slots matters: a game quit before the
	//ring had filled would otherwise leave freed pointers behind for the next
	//game's cleanup to free a second time.
	{
		u8 i;
		for(i=0; i <= PERSFRAMES; i++)
			if(vectors_pers[i] != NULL){
				free(vectors_pers[i]);
				vectors_pers[i] = NULL;
			}
		persCycle = 0; persFull = 0;
	}
}


int main(int argc, char *argv[]){
	GRRLIB_Init();
	WPAD_Init();
	PAD_Init();
	fatInitDefault();
	sound_init();

	//Fit to Wii's resolution
	scl_factor = ALG_MAX_Y / 444;
	offx = (640 - ALG_MAX_X / scl_factor) / 2;
	offy = (480 - ALG_MAX_Y / scl_factor) / 2;
	
	//Load splashscreen, font and Minestorm
	splash = GRRLIB_LoadTexture(splashscreen_png);
	myFont = GRRLIB_LoadTTF(Font_ttf, Font_ttf_size);
	memcpy (rom, rom_dat, sizeof(rom));	 //Preloaded/Default ROM [Minestorm]	
	
	while(1) {
		Menu();
		osint_emuloop(); 
	}
	return 0;	
}

