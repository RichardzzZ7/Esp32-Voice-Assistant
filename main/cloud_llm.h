#ifndef _CLOUD_LLM_H_
#define _CLOUD_LLM_H_

#include <stdbool.h>

typedef enum {
    LLM_ACTION_ADD,
    LLM_ACTION_REMOVE
} llm_action_t;

// Initialize cloud LLM service (if needed)
void cloud_llm_init(void);

// Send text to Cloud LLM for semantic parsing and inventory update
// Example text: "放入一瓶酸奶"
// Returns true if successful
bool cloud_llm_parse_inventory(const char *text, llm_action_t action);

// Recommend recipes based on current inventory
bool cloud_llm_recommend_recipes(void);

#endif // _CLOUD_LLM_H_
