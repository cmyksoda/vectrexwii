#define PERSFRAMES 20

void blurDot(u16 x, u16 y, u16 factor, u16 lightf, u32 col);
void blurLine(u16 x1, u16 y1, u16 x2, u16 y2, u16 factor, u32 col);

/* Batched point/line drawing (visuals.c) -- collects vertices instead of
 * issuing one GX_Begin/GX_End per point/line, then flushes as one draw call
 * per primitive type. Must be flushed before the blend mode changes or the
 * frame is presented; see the comment above vbatch_flush_points() in
 * visuals.c for why this doesn't change any pixel's final color. */
void vbatch_point(f32 x, f32 y, u32 color);
void vbatch_line(f32 x1, f32 y1, f32 x2, f32 y2, u32 color);
void vbatch_flush(void);

//wii_vector_type is exactly the same but using smaller variables to hold the coordinates and it does not store the color
typedef struct wii_vector_type {
	u16 x0, y0; /* computed start coordinate (on wiis resolution)*/
	u16 x1, y1; /* computed end coordinate (on wiis resolution)*/
} wii_vector_t;

wii_vector_t * vectors_pers[PERSFRAMES + 1];
long vector_pers_cnt[PERSFRAMES + 1];

u8 persFull = 0, persCycle = 0;
u8 optOverlay[2] = {1, 90}; //Enabled and opacity/alpha (90/255 = 35%)
u8 optPersistence[4] = {1, 2, 90, 135}; //Enabled, persistent frames, Grayscale (35%) and opacity/alpha (52%)
u8 optVtxCustomColor[4] = {1, 255, 255, 255}; //Enabled and RGB components of the custom color
u8 optGlow[3] = {1, 3, 90}; //Enabled, blur factor, opacity/alpha (90/255 = 35%)
u8 optScreenSize = 255; //Overscan correction: 255 = full, lower scales the picture down toward center
u8 pauseMenu[2] = {0, 1};