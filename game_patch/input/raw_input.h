#pragma once

void raw_input_start();
void raw_input_stop();
bool raw_input_is_running();
void raw_input_consume_deltas(int& dx, int& dy);
void raw_input_set_focused(bool focused);
// Name of the first device that sent absolute coordinates (remote desktop, VM, tablet), or nullptr.
// Such devices produce no relative deltas, so raw input cannot drive mouselook from them.
const char* raw_input_absolute_device();
int raw_input_get_event_count();
