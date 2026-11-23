#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "node_time.h"
#define EVENT_SERVICE_CAPACITY 32
/* Flags: source-valid 1, mapped 2, sequence gap 4, time regression 8. */
typedef struct {uint32_t session,sequence;uint64_t source_us,local_us;uint32_t uncertainty_us;uint8_t edge,flags;} service_event_t;
typedef struct {
 service_event_t ring[EVENT_SERVICE_CAPACITY];uint32_t session,last_sequence;
 uint64_t last_time;uint32_t accepted,duplicates,stale,gaps,regressions,overwritten;
 uint8_t count,next;
 uint64_t published; /* local journal position, independent of C5 reboot/sequence */
} event_service_t;
bool event_service_push(event_service_t *s,const service_event_t *event,const node_time_t *map,uint64_t now);
bool event_service_get(const event_service_t *s,unsigned newest,service_event_t *event);
void event_service_page(const event_service_t *s,unsigned page,uint8_t out[32]);

typedef struct {event_service_t events;uint32_t revision;uint64_t captured_us;} event_snapshot_t;
void event_snapshot_capture(event_snapshot_t *snapshot,const event_service_t *events,uint64_t now);
bool event_snapshot_page(const event_snapshot_t *snapshot,unsigned page,uint64_t now,uint8_t out[32]);

/* Consumer owns cursor, initialized to zero to replay retained history.
 * Peek does not advance it. Commit the returned position after processing.
 * Caller serializes all access with the producer lock. Recreate cursors when
 * the controller restarts; RAM journal positions are not durable identities. */
typedef struct {uint64_t position,lost;bool reset;} event_delivery_t;
bool event_service_peek(const event_service_t *service,uint64_t cursor,service_event_t *event,event_delivery_t *delivery);
