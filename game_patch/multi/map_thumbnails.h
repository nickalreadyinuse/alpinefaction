#pragma once

#include <string>

// FactionFiles preview images for levels, fetched in the background and kept as small bitmaps.
// Main thread only.

// Bitmap handle of the level's preview (out_w/out_h get its size), or -1 while it is still
// being fetched or when FactionFiles has none. The first call for a level queues the fetch;
// levels are fetched one at a time in request order, and a lookup is attempted once per session.
int map_thumbnail_get(const std::string& level_filename, int* out_w, int* out_h);

// Releases the bitmaps. The downloaded images stay cached, so a later map_thumbnail_get
// rebuilds the bitmap without asking FactionFiles again.
void map_thumbnails_release_bitmaps();
