#ifndef CHAT_SERVER_H
#define CHAT_SERVER_H

#include "chat.h"

struct chat_server;

/**
 * Create a new server.
 */
struct chat_server *
chat_server_new(void);

/**
 * Delete a server.
 */
void
chat_server_delete(struct chat_server *server);

/**
 * Start listening on a given port. 0 means any port, check
 * chat_server_get_port() to find out which one was chosen.
 *
 * @retval 0 Success.
 * @retval -1 Error, check errno.
 */
int
chat_server_listen(struct chat_server *server, uint16_t port);

/**
 * Get the server's socket descriptor.
 */
int
chat_server_get_socket(const struct chat_server *server);

/**
 * Get events supported by the server.
 */
int
chat_server_get_events(const struct chat_server *server);

/**
 * Update server's state. Process incoming connections, read/write
 * data from/to clients.
 *
 * @param timeout Timeout in seconds to wait for updates, can be 0
 *   to return immediately, or < 0 to wait indefinitely.
 *
 * @retval 0 Success, has updates.
 * @retval CHAT_ERR_TIMEOUT No updates within the timeout.
 * @retval CHAT_ERR_NOT_STARTED Server isn't started yet.
 * @retval -1 Error, see errno.
 */
int
chat_server_update(struct chat_server *server, double timeout);

/**
 * Get next unprocessed message from a client. Messages are queued
 * when they are received from clients in chat_server_update().
 *
 * @retval not NULL Success, a new message.
 * @retval NULL No new messages.
 */
struct chat_message *
chat_server_pop_next(struct chat_server *server);

/**
 * Get server descriptor for using in the select() function. This
 * descriptor becomes active on input when there are messages
 * from its socket or its clients. For a simple file descriptor
 * for using in poll() just use chat_server_get_socket().
 *
 * @retval >= 0 The descriptor.
 * @retval -1 Not implemented.
 */
int
chat_server_get_descriptor(const struct chat_server *server);

/**
 * Feed a message from server to all connected clients.
 *
 * @param msg Message text.
 * @param msg_size Message text size.
 *
 * @retval 0 Success.
 * @retval CHAT_ERR_NOT_IMPLEMENTED Not implemented.
 */
int
chat_server_feed(struct chat_server *server, const char *msg, uint32_t msg_size);

#endif /* CHAT_SERVER_H */
