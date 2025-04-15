#ifndef CHAT_CLIENT_H
#define CHAT_CLIENT_H

#include "chat.h"

struct chat_client;

/**
 * Create a new client.
 *
 * @param name Client name. Can be NULL for "anonymous".
 * @retval not NULL New client.
 */
struct chat_client *
chat_client_new(const char *name);

/**
 * Delete a client.
 */
void
chat_client_delete(struct chat_client *client);

/**
 * Connect to a server.
 *
 * @param addr Address in the format "host:port".
 * @retval 0 Success.
 * @retval -1 Error, check errno.
 */
int
chat_client_connect(struct chat_client *client, const char *addr);

/**
 * Get client socket descriptor.
 */
int
chat_client_get_descriptor(const struct chat_client *client);

/**
 * Get events wanted by the client.
 */
int
chat_client_get_events(const struct chat_client *client);

/**
 * Update client state. Process input/output.
 *
 * @param timeout Timeout in seconds to wait for updates, can be 0
 *   to return immediately, or < 0 to wait indefinitely.
 *
 * @retval 0 Success, has updates.
 * @retval CHAT_ERR_TIMEOUT No updates within the timeout.
 * @retval CHAT_ERR_NOT_STARTED Client isn't connected yet.
 * @retval -1 Error, see errno.
 */
int
chat_client_update(struct chat_client *client, double timeout);

/**
 * Get next unprocessed message from the server.
 *
 * @retval not NULL Success, a new message.
 * @retval NULL No new messages.
 */
struct chat_message *
chat_client_pop_next(struct chat_client *client);

/**
 * Feed a message to be sent to the server.
 *
 * @param msg Message text.
 * @param msg_size Message text size.
 *
 * @retval 0 Success.
 * @retval -1 Error, check errno.
 */
int
chat_client_feed(struct chat_client *client, const char *msg, uint32_t msg_size);

#endif /* CHAT_CLIENT_H */
