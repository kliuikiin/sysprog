#include "chat.h"
#include "chat_client.h"

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>

#define BUFFER_SIZE 4096
#define MAX_MESSAGES 128

struct chat_buffer {
	char *data;         /* Buffer data */
	uint32_t size;      /* Buffer capacity */
	uint32_t used;      /* Used bytes */
	uint32_t processed; /* Processed bytes (for output buffer) */
};

struct chat_message_queue {
	struct chat_message **messages;
	uint32_t capacity;
	uint32_t size;
	uint32_t read_pos;
};

struct chat_client {
	/** Socket connected to the server. */
	int socket;
	/** Input buffer from server. */
	struct chat_buffer input_buffer;
	/** Output buffer to server. */
	struct chat_buffer output_buffer;
	/** Message queue for received messages. */
	struct chat_message_queue message_queue;
	/** Client name */
#if NEED_AUTHOR
	char *name;
#endif
	/** Whether we have sent our name to the server */
	bool name_sent;
};

static int
set_nonblocking(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags == -1)
		return -1;
	return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void
chat_buffer_init(struct chat_buffer *buffer, uint32_t initial_size)
{
	buffer->data = malloc(initial_size);
	buffer->size = initial_size;
	buffer->used = 0;
	buffer->processed = 0;
}

static void
chat_buffer_free(struct chat_buffer *buffer)
{
	free(buffer->data);
	buffer->data = NULL;
	buffer->size = 0;
	buffer->used = 0;
	buffer->processed = 0;
}

static void
chat_buffer_ensure_capacity(struct chat_buffer *buffer, uint32_t additional_size)
{
	if (buffer->used + additional_size <= buffer->size)
		return;
	
	uint32_t new_size = buffer->size * 2;
	while (buffer->used + additional_size > new_size)
		new_size *= 2;
	
	buffer->data = realloc(buffer->data, new_size);
	buffer->size = new_size;
}

static void __attribute__((unused))
chat_buffer_compact(struct chat_buffer *buffer)
{
	if (buffer->processed == 0)
		return;
	
	if (buffer->processed == buffer->used) {
		buffer->used = 0;
		buffer->processed = 0;
		return;
	}
	
	memmove(buffer->data, buffer->data + buffer->processed, buffer->used - buffer->processed);
	buffer->used -= buffer->processed;
	buffer->processed = 0;
}

static void
chat_message_queue_init(struct chat_message_queue *queue, uint32_t initial_capacity)
{
	queue->messages = malloc(initial_capacity * sizeof(struct chat_message *));
	queue->capacity = initial_capacity;
	queue->size = 0;
	queue->read_pos = 0;
}

static void
chat_message_queue_free(struct chat_message_queue *queue)
{
	for (uint32_t i = queue->read_pos; i < queue->size; i++) {
		chat_message_delete(queue->messages[i]);
	}
	free(queue->messages);
	queue->messages = NULL;
	queue->capacity = 0;
	queue->size = 0;
	queue->read_pos = 0;
}

static void
chat_message_queue_ensure_capacity(struct chat_message_queue *queue)
{
	if (queue->size < queue->capacity)
		return;
	
	uint32_t new_capacity = queue->capacity * 2;
	queue->messages = realloc(queue->messages, new_capacity * sizeof(struct chat_message *));
	queue->capacity = new_capacity;
}

static void
chat_message_queue_push(struct chat_message_queue *queue, struct chat_message *message)
{
	chat_message_queue_ensure_capacity(queue);
	queue->messages[queue->size++] = message;
}

static struct chat_message *
chat_message_queue_pop(struct chat_message_queue *queue)
{
	if (queue->read_pos >= queue->size)
		return NULL;
	
	return queue->messages[queue->read_pos++];
}

static void
chat_message_queue_compact(struct chat_message_queue *queue)
{
	if (queue->read_pos == 0)
		return;
	
	if (queue->read_pos == queue->size) {
		queue->size = 0;
		queue->read_pos = 0;
		return;
	}
	
	uint32_t remaining = queue->size - queue->read_pos;
	memmove(queue->messages, queue->messages + queue->read_pos, remaining * sizeof(struct chat_message *));
	queue->size = remaining;
	queue->read_pos = 0;
}

static struct chat_message *
create_message(const char *data, uint32_t len, const char *author)
{
	// Skip leading whitespace
	while (len > 0 && isspace((unsigned char)*data)) {
		data++;
		len--;
	}
	
	// Skip trailing whitespace
	while (len > 0 && isspace((unsigned char)data[len - 1])) {
		len--;
	}
	
	if (len == 0)
		return NULL;  // Empty message after trimming
	
	struct chat_message *message = malloc(sizeof(struct chat_message));
	message->data = malloc(len + 1);
	memcpy(message->data, data, len);
	message->data[len] = '\0';
	
#if NEED_AUTHOR
	if (author) {
		message->author = strdup(author);
	} else {
		message->author = strdup("");
	}
#endif
	
	return message;
}

static void
chat_client_handle_server_input(struct chat_client *client)
{
	char buf[BUFFER_SIZE];
	ssize_t bytes_read;
	
	// Read as much data as possible
	while ((bytes_read = read(client->socket, buf, sizeof(buf))) > 0) {
		chat_buffer_ensure_capacity(&client->input_buffer, bytes_read);
		memcpy(client->input_buffer.data + client->input_buffer.used, buf, bytes_read);
		client->input_buffer.used += bytes_read;
	}
	
	if (bytes_read < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
		perror("read from server");
		// Error will be handled on next update
		return;
	}
	
	if (bytes_read == 0) {  // Server has closed the connection
		close(client->socket);
		client->socket = -1;
		return;
	}
	
	// Process complete messages
	char *data = client->input_buffer.data;
	uint32_t start = 0;
	
	for (uint32_t i = 0; i < client->input_buffer.used; i++) {
		if (data[i] == '\n') {
			// Extract author and message
			char *msg_start = data + start;
			uint32_t msg_len = i - start;
			
#if NEED_AUTHOR
			// Format is "author: message"
			char *colon = memchr(msg_start, ':', msg_len);
			if (colon) {
				uint32_t author_len = colon - msg_start;
				char *author = malloc(author_len + 1);
				memcpy(author, msg_start, author_len);
				author[author_len] = '\0';
				
				// Trim whitespace from author
				char *author_end = author + author_len - 1;
				while (author_end > author && isspace((unsigned char)*author_end))
					*author_end-- = '\0';
				
				// Skip colon and possible space
				msg_start = colon + 1;
				msg_len = i - (msg_start - data);
				
				// Trim leading whitespace from message
				while (msg_len > 0 && isspace((unsigned char)*msg_start)) {
					msg_start++;
					msg_len--;
				}
				
				struct chat_message *message = create_message(msg_start, msg_len, author);
				if (message) {
					chat_message_queue_push(&client->message_queue, message);
				}
				
				free(author);
			} else {
				// No author separator, use the whole message
				struct chat_message *message = create_message(msg_start, msg_len, "");
				if (message) {
					chat_message_queue_push(&client->message_queue, message);
				}
			}
#else
			struct chat_message *message = create_message(msg_start, msg_len, NULL);
			if (message) {
				chat_message_queue_push(&client->message_queue, message);
			}
#endif
			
			start = i + 1;
		}
	}
	
	// Move any remaining partial message to the beginning of the buffer
	if (start > 0) {
		memmove(data, data + start, client->input_buffer.used - start);
		client->input_buffer.used -= start;
	}
}

static void
chat_client_handle_output(struct chat_client *client)
{
	if (client->output_buffer.used == client->output_buffer.processed)
		return;
	
	ssize_t bytes_written = write(client->socket, 
								 client->output_buffer.data + client->output_buffer.processed,
								 client->output_buffer.used - client->output_buffer.processed);
	
	if (bytes_written > 0) {
		client->output_buffer.processed += bytes_written;
		
		// If we've written everything, reset the buffer
		if (client->output_buffer.processed == client->output_buffer.used) {
			client->output_buffer.used = 0;
			client->output_buffer.processed = 0;
		}
	} else if (bytes_written < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
		perror("write to server");
		// Error will be handled on next update
	}
}

struct chat_client *
chat_client_new(const char *name)
{
	struct chat_client *client = calloc(1, sizeof(*client));
	client->socket = -1;
	chat_buffer_init(&client->input_buffer, BUFFER_SIZE);
	chat_buffer_init(&client->output_buffer, BUFFER_SIZE);
	chat_message_queue_init(&client->message_queue, MAX_MESSAGES);
	client->name_sent = false;
	
#if NEED_AUTHOR
	if (name) {
		client->name = strdup(name);
	} else {
		client->name = strdup("anonymous");
	}
#endif
	
	return client;
}

void
chat_client_delete(struct chat_client *client)
{
	if (client->socket >= 0)
		close(client->socket);
	
	chat_buffer_free(&client->input_buffer);
	chat_buffer_free(&client->output_buffer);
	chat_message_queue_free(&client->message_queue);
	
#if NEED_AUTHOR
	free(client->name);
#endif
	
	free(client);
}

int
chat_client_connect(struct chat_client *client, const char *addr)
{
	char host[256];
	char *port_str;
	
	// Parse host:port
	strncpy(host, addr, sizeof(host) - 1);
	host[sizeof(host) - 1] = '\0';
	
	port_str = strchr(host, ':');
	if (!port_str) {
		return -1;
	}
	
	*port_str = '\0';
	port_str++;
	
	// Get address info
	struct addrinfo hints;
	struct addrinfo *result, *rp;
	
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	
	int rc = getaddrinfo(host, port_str, &hints, &result);
	if (rc != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rc));
		return -1;
	}
	
	// Try each address until we successfully connect
	for (rp = result; rp != NULL; rp = rp->ai_next) {
		client->socket = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (client->socket == -1)
			continue;
		
		// Set non-blocking mode
		if (set_nonblocking(client->socket) < 0) {
			close(client->socket);
			client->socket = -1;
			continue;
		}
		
		if (connect(client->socket, rp->ai_addr, rp->ai_addrlen) == 0 || errno == EINPROGRESS) {
			break; // Success or in progress
		}
		
		close(client->socket);
		client->socket = -1;
	}
	
	freeaddrinfo(result);
	
	if (client->socket == -1) {
		return -1;
	}
	
#if NEED_AUTHOR
	// Send the client name immediately
	if (!client->name_sent) {
		int name_len = strlen(client->name);
		chat_buffer_ensure_capacity(&client->output_buffer, name_len + 1);
		memcpy(client->output_buffer.data + client->output_buffer.used, client->name, name_len);
		client->output_buffer.used += name_len;
		client->output_buffer.data[client->output_buffer.used++] = '\n';
		client->name_sent = true;
	}
#endif
	
	return 0;
}

struct chat_message *
chat_client_pop_next(struct chat_client *client)
{
	struct chat_message *message = chat_message_queue_pop(&client->message_queue);
	
	// Compact the queue if needed
	if (client->message_queue.read_pos > client->message_queue.size / 2) {
		chat_message_queue_compact(&client->message_queue);
	}
	
	return message;
}

int
chat_client_update(struct chat_client *client, double timeout)
{
	if (client->socket < 0) {
		return CHAT_ERR_NOT_STARTED;
	}
	
	struct pollfd pfd;
	pfd.fd = client->socket;
	pfd.events = POLLIN;
	
	if (client->output_buffer.used > client->output_buffer.processed) {
		pfd.events |= POLLOUT;
	}
	
	int timeout_ms = timeout < 0 ? -1 : (int)(timeout * 1000);
	int event_count = poll(&pfd, 1, timeout_ms);
	
	if (event_count < 0) {
		if (errno == EINTR) {
			return 0;
		}
		perror("poll");
		return -1;
	}
	
	if (event_count == 0) {
		return CHAT_ERR_TIMEOUT;
	}
	
	if (pfd.revents & POLLIN) {
		chat_client_handle_server_input(client);
	}
	
	if (pfd.revents & POLLOUT) {
		chat_client_handle_output(client);
	}
	
	if (pfd.revents & (POLLHUP | POLLERR)) {
		// Server disconnected or error
		close(client->socket);
		client->socket = -1;
		return -1;
	}
	
	return 0;
}

int
chat_client_get_descriptor(const struct chat_client *client)
{
	return client->socket;
}

int
chat_client_get_events(const struct chat_client *client)
{
	// If client is not connected yet, return no events
	if (client->socket < 0) {
		return 0;
	}
	
	int events = CHAT_EVENT_INPUT;
	
	if (client->output_buffer.used > client->output_buffer.processed) {
		events |= CHAT_EVENT_OUTPUT;
	}
	
	return events;
}

int
chat_client_feed(struct chat_client *client, const char *msg, uint32_t msg_size)
{
	chat_buffer_ensure_capacity(&client->output_buffer, msg_size);
	memcpy(client->output_buffer.data + client->output_buffer.used, msg, msg_size);
	client->output_buffer.used += msg_size;
	
	return 0;
}
