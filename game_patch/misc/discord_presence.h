#pragma once

#include "../rf/multi.h"

void discord_presence_init();
void discord_presence_do_frame();
void discord_presence_shutdown();
// Remembers whether the join request to this server carried a password, so the presence never
// advertises a join link for a passworded server.
void discord_presence_on_join_request(const rf::NetAddr& addr, bool has_password);
