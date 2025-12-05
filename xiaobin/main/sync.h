// sync.h - offline queue and synchronization to cloud
#ifndef _SYNC_H_
#define _SYNC_H_

#include <stdbool.h>

void sync_init(void);
// Enqueue an event: event_type (e.g., "add_item", "mark_notified"), payload is JSON string
int sync_enqueue_event(const char *event_type, const char *payload_json);

#endif // _SYNC_H_
