#ifndef __ROM_CATALOG_H
#define __ROM_CATALOG_H

#include <stdint.h>
#include <stddef.h>

/* One bundled game: its display title, its embedded .vec image (rom/rom_size)
 * and its embedded overlay PNG bytes (overlay, NULL when the game never had an
 * overlay -- the demos never did). No overlay_size: GRRLIB_LoadTexture parses
 * the PNG header itself, exactly like the built-in Minestorm overlay. */
typedef struct {
	const char *title;
	const uint8_t *rom;
	size_t rom_size;
	const uint8_t *overlay;
} CatalogEntry;

extern const CatalogEntry catalog_official[];
extern const int catalog_official_count;
extern const CatalogEntry catalog_mods[];
extern const int catalog_mods_count;
extern const CatalogEntry catalog_prototypes[];
extern const int catalog_prototypes_count;
extern const CatalogEntry catalog_demos[];
extern const int catalog_demos_count;

#endif
