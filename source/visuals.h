#define PERSFRAMES 20

void blurDot(u16 x, u16 y, u16 factor, u16 lightf, u32 col);
void blurLine(u16 x1, u16 y1, u16 x2, u16 y2, u16 factor, u32 col);

//wii_vector_type is exactly the same but using smaller variables to hold the coordinates and it does not store the color
typedef struct wii_vector_type {
	u16 x0, y0; /* computed start coordinate (on wiis resolution)*/
	u16 x1, y1; /* computed end coordinate (on wiis resolution)*/
} wii_vector_t;

wii_vector_t * vectors_pers[PERSFRAMES + 1];
long vector_pers_cnt[PERSFRAMES + 1];

u8 persFull = 0, persCycle = 0;
u8 optOverlay[2] = {1, 135}; //Enabled and opacity/alpha (135/255 ~= 53%, slider value 9 of 17)
u8 optPersistence[4] = {1, 8, 30 , 255}; //Enabled, persistent frames, Grayscale and opacity/alpha
u8 optVtxCustomColor[4] = {1, 255, 255, 255}; //Enabled and RGB components of the custom color
u8 optGlow[2] = {1, 6};
u8 optScreenSize = 255; //Overscan correction: 255 = full, lower scales the picture down toward center
u8 pauseMenu[2] = {0, 1};