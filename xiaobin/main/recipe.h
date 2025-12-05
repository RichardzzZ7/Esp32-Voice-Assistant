// recipe.h - recipe recommendation client (cloud + local fallback)
#ifndef _RECIPE_H_
#define _RECIPE_H_

#include "inventory.h"

// Request recipe suggestions for the given items (array of pointers), non-blocking.
// The module will speak the top suggestion via TTS and store the suggestion in SPIFFS.
void recipe_request_for_items(inventory_item_t **items, int count);

#endif // _RECIPE_H_
