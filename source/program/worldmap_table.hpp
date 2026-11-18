/* Generated from Data/WorldMap.xnb + Maps tbin files (PC 1.6.15).
 * rect is in world-map texture pixels; the game draws it at 4x.
 * Cross-checked by tools/check_worldmap_table.py (rect + region).  Two rows
 * are hand-written because the game gives those locations no position at all
 * -- they are marked below and in that script.
 *
 * v554: Data/WorldMap holds two regions, "Valley" and "GingerIsland", and
 * the map page draws exactly one of them.  MapPage.ctor picks the region of
 * the player's current location (falling back to the Farm), and vanilla
 * drawMiniPortraits skips every farmer whose position resolves into another
 * region -- so every row carries its region and the avatar pass only draws
 * rows whose region is the one on screen.
 *
 * Both regions sit on the same 300x180 map-pixel grid and are drawn at the
 * same 4x scale (MapRegion.GetMapPixelBounds divides the 4x MapPixelArea by
 * 4 again), so rect/mapW/mapH mean the same thing in either region.
 *
 * A 1x1 rect means a fixed position: MapAreaPosition.GetMapPixelPosition
 * uses the centre of that pixel and ignores the tile, which is what the
 * avatar pass does for these rows too (Mines, Mine, Summit, LeoTreeHouse,
 * IslandFarmHouse).
 *
 * v555: two rows have no position in Data/WorldMap and are synthesized so
 * their characters do not fall into the Farm fallback:
 *   LeoTreeHouse  the door the game stamps onto the Mountain map at (16,7)
 *                 once leoMoved is set, mapped through the Mountain row;
 *   Mine          vanilla's own alias ("Mine" -> "Mines"), where the Dwarf
 *                 lives. */
#pragma once

enum WorldMapRegion {
    kMapRegionValley = 0,
    kMapRegionIsland = 1
};

struct WorldMapEntry {
    const char* name;
    short rectX, rectY, rectW, rectH;
    short mapW, mapH;
    unsigned char region;   /* WorldMapRegion */
};

static const WorldMapEntry kWorldMap[] = {
    /* ---- Valley ------------------------------------------------------ */
    { "AbandonedJojaMart", 215, 79, 7, 5, 18, 17, kMapRegionValley },
    { "AdventureGuild", 219, 28, 3, 2, 14, 20, kMapRegionValley },
    { "AnimalShop", 102, 102, 2, 2, 35, 20, kMapRegionValley },
    { "ArchaeologyHouse", 220, 110, 5, 3, 50, 20, kMapRegionValley },
    { "Backwoods", 82, 38, 29, 24, 50, 40, kMapRegionValley },
    { "BathHouse_Entry", 146, 20, 3, 3, 10, 10, kMapRegionValley },
    { "BathHouse_MensLocker", 146, 17, 3, 3, 18, 28, kMapRegionValley },
    { "BathHouse_Pool", 149, 17, 6, 6, 28, 34, kMapRegionValley },
    { "BathHouse_WomensLocker", 146, 17, 3, 3, 18, 28, kMapRegionValley },
    { "Beach", 186, 137, 57, 31, 104, 50, kMapRegionValley },
    { "Blacksmith", 214, 104, 3, 3, 15, 20, kMapRegionValley },
    { "BugLand", 112, 151, 2, 2, 60, 60, kMapRegionValley },
    { "BusStop", 115, 50, 30, 29, 65, 30, kMapRegionValley },
    { "Club", 36, 20, 1, 1, 50, 60, kMapRegionValley },
    { "BeachNightMarket", 186, 137, 57, 31, 104, 50, kMapRegionValley },
    { "CommunityCenter", 184, 57, 5, 4, 25, 25, kMapRegionValley },
    { "Desert", 36, 20, 1, 1, 50, 60, kMapRegionValley },
    { "DesertFestival", 36, 20, 1, 1, 50, 60, kMapRegionValley },
    { "ElliottHouse", 211, 140, 2, 3, 16, 10, kMapRegionValley },
    { "Farm", 68, 62, 50, 34, 80, 65, kMapRegionValley },
    { "FarmCave", 98, 63, 1, 2, 80, 65, kMapRegionValley },
    { "FishShop", 202, 155, 5, 3, 12, 10, kMapRegionValley },
    { "Forest", 47, 96, 76, 65, 120, 120, kMapRegionValley },
    { "HaleyHouse", 162, 108, 5, 3, 25, 30, kMapRegionValley },
    { "HarveyRoom", 171, 84, 3, 3, 24, 13, kMapRegionValley },
    { "Hospital", 171, 84, 3, 3, 24, 20, kMapRegionValley },
    { "JojaMart", 215, 79, 7, 5, 30, 30, kMapRegionValley },
    { "JoshHouse", 188, 89, 4, 4, 25, 25, kMapRegionValley },
    { "LeahHouse", 111, 113, 3, 2, 16, 16, kMapRegionValley },
    /* v555: Leo's tree house after `leoMoved`.  It has no position in
     * Data/WorldMap at all -- the game stamps the door onto the *Mountain*
     * map at tile (16,7) (Mountain.ApplyTreehouseIfNecessary), so the rect is
     * that tile through the Mountain row's own mapping
     * (165 + 98*16/135 = 176.6, 26 + 18*7/41 = 29.1), 1x1 so the head sits on
     * the door.  Without this row the Farm fallback put Leo in the middle of
     * the valley farm. */
    { "LeoTreeHouse", 176, 29, 1, 1, 135, 41, kMapRegionValley },
    { "ManorHouse", 189, 104, 6, 6, 25, 12, kMapRegionValley },
    { "MermaidHouse", 213, 155, 5, 3, 9, 12, kMapRegionValley },
    /* v555: the mine entrance interior, where the Dwarf lives.  Vanilla's own
     * MapRegion.GetLocationName maps the location name "Mine" to "Mines"
     * (`if (text == "Mine") return "Mines";`), so give it the Mines pixel
     * instead of the Farm fallback. */
    { "Mine", 204, 26, 1, 1, 135, 41, kMapRegionValley },
    { "Mines", 204, 26, 1, 1, 135, 41, kMapRegionValley },
    { "Mountain", 165, 26, 98, 18, 135, 41, kMapRegionValley },
    { "MovieTheater", 180, 54, 10, 6, 23, 17, kMapRegionValley },
    { "MovieTheaterScreen", 180, 54, 10, 6, 14, 17, kMapRegionValley },
    { "Railroad", 140, 10, 46, 15, 70, 62, kMapRegionValley },
    { "Saloon", 177, 95, 4, 3, 46, 25, kMapRegionValley },
    { "SamHouse", 153, 107, 3, 3, 25, 25, kMapRegionValley },
    { "SandyHouse", 36, 20, 1, 1, 50, 60, kMapRegionValley },
    { "SandyShop", 36, 20, 1, 1, 50, 60, kMapRegionValley },
    { "ScienceHouse", 172, 36, 6, 4, 32, 25, kMapRegionValley },
    { "SebastianRoom", 172, 36, 6, 4, 13, 13, kMapRegionValley },
    { "SeedShop", 175, 84, 6, 3, 48, 32, kMapRegionValley },
    { "Sewer", 175, 120, 2, 2, 40, 50, kMapRegionValley },
    { "SkullCave", 36, 20, 1, 1, 50, 60, kMapRegionValley },
    { "Submarine", 181, 158, 9, 8, 25, 18, kMapRegionValley },
    { "Summit", 161, 0, 1, 1, 70, 62, kMapRegionValley },
    { "Sunroom", 176, 83, 6, 4, 12, 15, kMapRegionValley },
    { "Tent", 189, 28, 1, 2, 135, 41, kMapRegionValley },
    { "Town", 147, 46, 97, 80, 130, 110, kMapRegionValley },
    { "Trailer", 198, 94, 5, 3, 20, 12, kMapRegionValley },
    { "Trailer_Big", 198, 94, 4, 3, 30, 26, kMapRegionValley },
    { "Tunnel", 109, 57, 1, 1, 50, 40, kMapRegionValley },
    { "WitchHut", 174, 7, 1, 1, 70, 62, kMapRegionValley },
    { "WitchSwamp", 174, 7, 1, 1, 70, 62, kMapRegionValley },
    { "WitchWarpCave", 174, 7, 1, 1, 70, 62, kMapRegionValley },
    { "WizardHouse", 50, 107, 2, 3, 14, 25, kMapRegionValley },
    { "WizardHouseBasement", 50, 107, 2, 3, 20, 8, kMapRegionValley },
    /* v555: the Secret Woods were missing from the generated table, so its
     * characters fell into the Farm fallback.  Rect from the SecretWoods
     * area (the location's own WorldPosition has an empty MapPixelArea), map
     * size from Maps\Woods.tbin (60x32). */
    { "Woods", 28, 81, 19, 10, 60, 32, kMapRegionValley },
    /* ---- Ginger Island ----------------------------------------------
     * The area rectangles are 31x39 / 52x66 / 31x44 / 31x44 / 80x80 on the
     * 300x180 island map (LooseSprites\map_island); the interiors ride on
     * small fixed rects.  mapW/mapH are the map sizes of the locations
     * themselves (Island_S, Island_N, Island_W, Island_E, Island_SE, ...),
     * i.e. what MapAreaPosition.GetPositionRatioIfValid divides the tile
     * by.  VolcanoDungeon's rect is 1x1, so its head is centred and the
     * size never enters the maths. */
    { "Caldera", 155, 33, 7, 4, 48, 48, kMapRegionIsland },
    { "CaptainRoom", 85, 149, 1, 1, 6, 8, kMapRegionIsland },
    { "IslandEast", 181, 65, 31, 39, 43, 60, kMapRegionIsland },
    { "IslandFarmCave", 106, 102, 5, 6, 10, 14, kMapRegionIsland },
    { "IslandFarmHouse", 95, 108, 1, 1, 30, 18, kMapRegionIsland },
    { "IslandHut", 190, 67, 13, 12, 16, 16, kMapRegionIsland },
    { "IslandNorth", 129, 38, 52, 66, 70, 90, kMapRegionIsland },
    { "IslandNorthCave1", 145, 71, 2, 2, 12, 12, kMapRegionIsland },
    { "IslandShrine", 210, 70, 5, 5, 48, 48, kMapRegionIsland },
    { "IslandSouth", 142, 104, 31, 44, 43, 60, kMapRegionIsland },
    { "IslandSouthEast", 174, 104, 31, 44, 43, 60, kMapRegionIsland },
    { "IslandSouthEastCave", 200, 113, 6, 12, 40, 41, kMapRegionIsland },
    { "IslandWest", 39, 82, 80, 80, 110, 110, kMapRegionIsland },
    { "IslandWestCave1", 81, 84, 3, 3, 12, 12, kMapRegionIsland },
    { "QiNutRoom", 51, 95, 5, 4, 15, 10, kMapRegionIsland },
    { "VolcanoDungeon", 159, 53, 1, 1, 16, 16, kMapRegionIsland },
};

static const int kWorldMapCount = sizeof(kWorldMap) / sizeof(kWorldMap[0]);
