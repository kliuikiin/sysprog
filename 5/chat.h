#ifndef CHAT_H
#define CHAT_H

#include <stdint.h>
#include <stdbool.h>

/**
 * For testing, define these to get more features and points.
 * NEED_AUTHOR - support client names (+5 points)
 * NEED_SERVER_FEED - support server messages (+5 points)
 */
#define NEED_AUTHOR 1
#define NEED_SERVER_FEED 1

/**
 * Error codes.
 */
enum {
	CHAT_ERR_TIMEOUT = 1,
	CHAT_ERR_NOT_IMPLEMENTED = 2,
	CHAT_ERR_NOT_STARTED = 3,
};

/**
 * Event types for wait functions.
 */
enum {
	CHAT_EVENT_INPUT = 1,
	CHAT_EVENT_OUTPUT = 2,
};

/**
 * A chat message.
 */
struct chat_message {
	/** Message text. */
	char *data;
#if NEED_AUTHOR
	/** Message author. */
	char *author;
#endif
};

/**
 * Delete a message created by any of chat_*_pop_next() functions.
 */
void
chat_message_delete(struct chat_message *msg);

/**
 * Convert a mask of CHAT_EVENT_* to a mask of POLL* for poll().
 */
int
chat_events_to_poll_events(int mask);

#endif /* CHAT_H */
