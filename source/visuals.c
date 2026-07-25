/************************************************************************
 * Pseudo bluring functions.
 *
 * These functions draw a close enough approach of the output
 * that blurring a texture would produce on a specific line/point.
 ***********************************************************************/

#include <grrlib.h>

void blurDot(u16 x, u16 y, u16 factor, u16 lightf, u32 col)
{
int div, i, j, ui;
u8 colors[3];

colors[0] = R(col);
colors[1] = G(col);
colors[2] = B(col);

div = 1 + (2 * lightf);

	for(i = -factor; i <= factor; i++)
	{
		if (i < 0) ui = factor+i;
		else ui = factor-i;

		//Plot a "square" with a 45 deg rotation
		for(j = -ui; j <= ui; j++)
		{
			GRRLIB_Plot(x+i, y+j, RGBA(colors[0]/div, colors[1]/div, colors[2]/div, 0xFF));
		}
	}
}

void blurLine(u16 x1, u16 y1, u16 x2, u16 y2, u16 factor, u32 col)
{
s16 slope[2]={y1-y2, x1-x2};
u16 i, div;
u8 colors[3];

colors[0] = R(col);
colors[1] = G(col);
colors[2] = B(col);

div = 1 + (2 * factor);

GRRLIB_Line(x1, y1, x2, y2, 0xFFFFFFFF);

	if(slope[0] == 0) //blur vertically
	{
		for(i=1; i <= factor; i++)
		{
			
			GRRLIB_Line(x1, y1-i, x2, y2-i, RGBA(colors[0]/div, colors[1]/div, colors[2]/div, 0xFF));
			GRRLIB_Line(x1, y1+i, x2, y2+i, RGBA(colors[0]/div, colors[1]/div, colors[2]/div, 0xFF));
		}
	}else{		
		for(i=1; i <= factor; i++)
		{
			GRRLIB_Line(x1-i, y1, x2-i, y2, RGBA(colors[0]/div, colors[1]/div, colors[2]/div, 0xFF));
			GRRLIB_Line(x1+i, y1, x2+i, y2, RGBA(colors[0]/div, colors[1]/div, colors[2]/div, 0xFF));
		}
	}
}