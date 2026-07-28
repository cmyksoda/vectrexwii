/************************************************************************
 * Pseudo bluring functions.
 *
 * These functions draw a close enough approach of the output
 * that blurring a texture would produce on a specific line/point.
 ***********************************************************************/

#include <grrlib.h>
/* Deliberately NOT including visuals.h here: it defines (not just declares)
 * global variables, so it may only be included by the one .c file that owns
 * them (osint.c) -- pulling it in here too would double-define them at link
 * time. Prototypes for the functions below live in visuals.h regardless. */

/* ---- batched point/line draw buffers -----------------------------------
 * GRRLIB_Plot/GRRLIB_Line each do their own GX_Begin/GX_End -- one GX draw
 * call per point or line. With persistence + glow, a single busy Vectrex
 * frame can issue tens of thousands of those. Real GX silicon shrugs this
 * off, but Dolphin's GX HLE pays a large fixed cost per GX_Begin/End, which
 * is exactly why this emulator was ~50x slower (and often unplayable) in
 * Dolphin while running fine on real hardware.
 *
 * GX_Begin/End already accepts N vertices per call (see GRRLIB_GXEngine),
 * so instead of drawing immediately, points and lines are appended here and
 * flushed as one GX_POINTS and one GX_LINES draw call per pass. This does
 * NOT change a single pixel: every caller in a given pass (persistence,
 * glow, or the final opaque draw) uses either a constant blend color+alpha
 * repeated under alpha-blend (order-independent -- it's the same affine map
 * applied N times), a saturating additive blend (order-independent because
 * clamp(clamp(a+b)+c) == clamp(a+b+c) for non-negative a,b,c), or a fully
 * opaque single color under alpha-blend (src replaces dst regardless of
 * order). Batching must still be flushed before the blend mode changes or
 * the frame renders, so the two are never mixed under the wrong mode. */
#define VBATCH_POINTS_MAX 16384
#define VBATCH_LINES_MAX  16384 /* line segments; 2 vertices each */

typedef struct { f32 x, y; u32 color; } vbatch_vtx_t;

static vbatch_vtx_t vbatch_pts[VBATCH_POINTS_MAX];
static u32 vbatch_pts_n;

static vbatch_vtx_t vbatch_ln[VBATCH_LINES_MAX * 2];
static u32 vbatch_ln_n;

static void vbatch_flush_points(void)
{
	u32 i;
	if (!vbatch_pts_n) return;
	GX_Begin(GX_POINTS, GX_VTXFMT0, vbatch_pts_n);
	for (i = 0; i < vbatch_pts_n; i++) {
		GX_Position3f32(vbatch_pts[i].x, vbatch_pts[i].y, 0.0f);
		GX_Color1u32(vbatch_pts[i].color);
	}
	GX_End();
	vbatch_pts_n = 0;
}

static void vbatch_flush_lines(void)
{
	u32 i, n;
	if (!vbatch_ln_n) return;
	n = vbatch_ln_n * 2;
	GX_Begin(GX_LINES, GX_VTXFMT0, n);
	for (i = 0; i < n; i++) {
		GX_Position3f32(vbatch_ln[i].x, vbatch_ln[i].y, 0.0f);
		GX_Color1u32(vbatch_ln[i].color);
	}
	GX_End();
	vbatch_ln_n = 0;
}

void vbatch_flush(void)
{
	vbatch_flush_points();
	vbatch_flush_lines();
}

void vbatch_point(f32 x, f32 y, u32 color)
{
	if (vbatch_pts_n >= VBATCH_POINTS_MAX) vbatch_flush_points();
	vbatch_pts[vbatch_pts_n].x = x;
	vbatch_pts[vbatch_pts_n].y = y;
	vbatch_pts[vbatch_pts_n].color = color;
	vbatch_pts_n++;
}

void vbatch_line(f32 x1, f32 y1, f32 x2, f32 y2, u32 color)
{
	if (vbatch_ln_n >= VBATCH_LINES_MAX) vbatch_flush_lines();
	vbatch_ln[vbatch_ln_n * 2 + 0].x = x1;
	vbatch_ln[vbatch_ln_n * 2 + 0].y = y1;
	vbatch_ln[vbatch_ln_n * 2 + 0].color = color;
	vbatch_ln[vbatch_ln_n * 2 + 1].x = x2;
	vbatch_ln[vbatch_ln_n * 2 + 1].y = y2;
	vbatch_ln[vbatch_ln_n * 2 + 1].color = color;
	vbatch_ln_n++;
}

/* col's own alpha channel is the glow opacity (osint.c builds it from
 * optGlow[2]) -- reusing it here instead of hardcoding 0xFF keeps the
 * additive-saturating-blend argument in vbatch_flush_points/lines' comment
 * intact: it's still one constant alpha applied throughout the whole glow
 * pass, just no longer pinned to fully-opaque. */
void blurDot(u16 x, u16 y, u16 factor, u16 lightf, u32 col)
{
int div, i, j, ui;
u8 colors[3], a;

colors[0] = R(col);
colors[1] = G(col);
colors[2] = B(col);
a = A(col);

div = 1 + (2 * lightf);

	for(i = -factor; i <= factor; i++)
	{
		if (i < 0) ui = factor+i;
		else ui = factor-i;

		//Plot a "square" with a 45 deg rotation
		for(j = -ui; j <= ui; j++)
		{
			vbatch_point(x+i, y+j, RGBA(colors[0]/div, colors[1]/div, colors[2]/div, a));
		}
	}
}

void blurLine(u16 x1, u16 y1, u16 x2, u16 y2, u16 factor, u32 col)
{
s16 slope[2]={y1-y2, x1-x2};
u16 i, div;
u8 colors[3], a;

colors[0] = R(col);
colors[1] = G(col);
colors[2] = B(col);
a = A(col);

div = 1 + (2 * factor);

vbatch_line(x1, y1, x2, y2, RGBA(0xFF, 0xFF, 0xFF, a));

	if(slope[0] == 0) //blur vertically
	{
		for(i=1; i <= factor; i++)
		{

			vbatch_line(x1, y1-i, x2, y2-i, RGBA(colors[0]/div, colors[1]/div, colors[2]/div, a));
			vbatch_line(x1, y1+i, x2, y2+i, RGBA(colors[0]/div, colors[1]/div, colors[2]/div, a));
		}
	}else{
		for(i=1; i <= factor; i++)
		{
			vbatch_line(x1-i, y1, x2-i, y2, RGBA(colors[0]/div, colors[1]/div, colors[2]/div, a));
			vbatch_line(x1+i, y1, x2+i, y2, RGBA(colors[0]/div, colors[1]/div, colors[2]/div, a));
		}
	}
}
