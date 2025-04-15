#include "chat.h"
#include "chat_server.h"

#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <ctype.h>
#include <netdb.h>
#include <errno.h>

#ifdef __linux__
#include <sys/epoll.h>
#define EPOLL_MODE
#elif defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/event.h>
#define KQUEUE_MODE
#else
#include <poll.h>
#define POLL_MODE
#endif

#define MAX_PEERS 1024
#define BUFFER_SIZE 4096
#define MAX_MESSAGES 128

struct chat_buffer {
	char *data;
	uint32_t size;
	uint32_t used;
	uint32_t processed;
};

struct chat_message_queue {
	struct chat_message **messages;
	uint32_t capacity;
	uint32_t size;
	uint32_t read_pos;
};

struct chat_peer {
	int socket;
	struct chat_buffer input_buffer;
	struct chat_buffer output_buffer;
#if NEED_AUTHOR
	char *name;
#endif
	bool is_active;
};

struct chat_server {
	int socket;
	int io_descriptor;
	struct chat_peer *peers;
	int peer_count;
	int peer_capacity;
	struct chat_message_queue message_queue;
#if NEED_SERVER_FEED
	struct chat_buffer server_buffer;
#endif
};

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
	while (len > 0 && isspace((unsigned char)*data)) {
		data++;
		len--;
	}
	
	while (len > 0 && isspace((unsigned char)data[len - 1])) {
		len--;
	}
	
	if (len == 0)
		return NULL;
	
	struct chat_message *message = malloc(sizeof(struct chat_message));
	message->data = malloc(len + 1);
	memcpy(message->data, data, len);
	message->data[len] = '\0';
	
#if NEED_AUTHOR
	if (author) {
		message->author = strdup(author);
	} else {
		message->author = strdup("server");
	}
#endif
	
	return message;
}

static int
set_nonblocking(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags == -1)
		return -1;
	return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void
chat_server_add_peer(struct chat_server *server, int socket)
{
	if (server->peer_count == server->peer_capacity) {
		int new_capacity = server->peer_capacity * 2;
		server->peers = realloc(server->peers, new_capacity * sizeof(struct chat_peer));
		server->peer_capacity = new_capacity;
	}
	
	struct chat_peer *peer = &server->peers[server->peer_count++];
	peer->socket = socket;
	chat_buffer_init(&peer->input_buffer, BUFFER_SIZE);
	chat_buffer_init(&peer->output_buffer, BUFFER_SIZE);
	peer->is_active = true;
	
#if NEED_AUTHOR
	peer->name = NULL;
#endif
	
#ifdef EPOLL_MODE
	struct epoll_event event;
	event.events = EPOLLIN | EPOLLOUT | EPOLLET;
	event.data.ptr = peer;
	if (epoll_ctl(server->io_descriptor, EPOLL_CTL_ADD, socket, &event) == -1) {
		perror("epoll_ctl: add peer");
		abort();
	}
#elif defined(KQUEUE_MODE)
	struct kevent events[2];
	EV_SET(&events[0], socket, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, peer);
	EV_SET(&events[1], socket, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, peer);
	if (kevent(server->io_descriptor, events, 2, NULL, 0, NULL) == -1) {
		perror("kevent: add peer");
		abort();
	}
#endif
}

static void
chat_server_remove_peer(struct chat_server *server, struct chat_peer *peer)
{
#ifdef EPOLL_MODE
	epoll_ctl(server->io_descriptor, EPOLL_CTL_DEL, peer->socket, NULL);
#elif defined(KQUEUE_MODE)
	struct kevent events[2];
	EV_SET(&events[0], peer->socket, EVFILT_READ, EV_DELETE, 0, 0, NULL);
	EV_SET(&events[1], peer->socket, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
	kevent(server->io_descriptor, events, 2, NULL, 0, NULL);
#endif
	
	close(peer->socket);
	chat_buffer_free(&peer->input_buffer);
	chat_buffer_free(&peer->output_buffer);
	
#if NEED_AUTHOR
	free(peer->name);
#endif
	
	peer->is_active = false;
}

static void
chat_server_broadcast_message(struct chat_server *server, struct chat_message *message, 
							  struct chat_peer *exclude_peer)
{
	int active_peers = 0;
	for (int i = 0; i < server->peer_count; i++) {
		if (server->peers[i].is_active && &server->peers[i] != exclude_peer) {
			active_peers++;
		}
	}
	
	for (int i = 0; i < server->peer_count; i++) {
		struct chat_peer *peer = &server->peers[i];
		
		if (!peer->is_active || peer == exclude_peer)
			continue;
		
		char *formatted_message;
#if NEED_AUTHOR
		int len = asprintf(&formatted_message, "%s: %s\n", message->author, message->data);
#else
		int len = asprintf(&formatted_message, "%s\n", message->data);
#endif
		
		if (len < 0) {
			perror("asprintf");
			continue;
		}
		
		chat_buffer_ensure_capacity(&peer->output_buffer, len);
		memcpy(peer->output_buffer.data + peer->output_buffer.used, formatted_message, len);
		peer->output_buffer.used += len;
		
		free(formatted_message);
	}
}

static void
chat_server_handle_client_input(struct chat_server *server, struct chat_peer *peer)
{
	char buf[BUFFER_SIZE];
	ssize_t bytes_read;
	
	while ((bytes_read = read(peer->socket, buf, sizeof(buf))) > 0) {
		chat_buffer_ensure_capacity(&peer->input_buffer, bytes_read);
		memcpy(peer->input_buffer.data + peer->input_buffer.used, buf, bytes_read);
		peer->input_buffer.used += bytes_read;
	}
	
	if (bytes_read < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
		perror("read");
		chat_server_remove_peer(server, peer);
		return;
	}
	
	if (bytes_read == 0) {
		chat_server_remove_peer(server, peer);
		return;
	}
	
	char *data = peer->input_buffer.data;
	uint32_t start = 0;
	
	for (uint32_t i = 0; i < peer->input_buffer.used; i++) {
		if (data[i] == '\n') {
#if NEED_AUTHOR
			if (peer->name == NULL) {
				uint32_t name_len = i - start;
				uint32_t trim_start = start;
				while (trim_start < i && isspace((unsigned char)data[trim_start])) {
					trim_start++;
				}
				uint32_t trim_end = i;
				while (trim_end > trim_start && isspace((unsigned char)data[trim_end - 1])) {
					trim_end--;
				}
				name_len = trim_end - trim_start;
				
				if (name_len == 0) {
					peer->name = strdup("anonymous");
				} else {
					peer->name = malloc(name_len + 1);
					memcpy(peer->name, data + trim_start, name_len);
					peer->name[name_len] = '\0';
				}
			} else
#endif
			{
				struct chat_message *message = create_message(data + start, i - start, 
#if NEED_AUTHOR
															 peer->name
#else
															 NULL
#endif
															 );
				if (message) {
					chat_message_queue_push(&server->message_queue, message);
					
					chat_server_broadcast_message(server, message, peer);
				}
			}
			start = i + 1;
		}
	}
	
	if (start > 0) {
		memmove(data, data + start, peer->input_buffer.used - start);
		peer->input_buffer.used -= start;
	}
}

static void
chat_server_handle_client_output(struct chat_peer *peer)
{
	if (peer->output_buffer.used == peer->output_buffer.processed)
		return;
	
	ssize_t bytes_written = write(peer->socket, 
								 peer->output_buffer.data + peer->output_buffer.processed,
								 peer->output_buffer.used - peer->output_buffer.processed);
	
	if (bytes_written > 0) {
		peer->output_buffer.processed += bytes_written;
		
		if (peer->output_buffer.processed == peer->output_buffer.used) {
			peer->output_buffer.used = 0;
			peer->output_buffer.processed = 0;
		}
	} else if (bytes_written < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
		perror("write");
	}
}

struct chat_server *
chat_server_new(void)
{
	struct chat_server *server = calloc(1, sizeof(*server));
	server->socket = -1;
	server->peer_capacity = MAX_PEERS;
	server->peers = malloc(server->peer_capacity * sizeof(struct chat_peer));
	server->peer_count = 0;
	chat_message_queue_init(&server->message_queue, MAX_MESSAGES);
	
#if NEED_SERVER_FEED
	chat_buffer_init(&server->server_buffer, BUFFER_SIZE);
#endif
	
#ifdef EPOLL_MODE
	server->io_descriptor = epoll_create1(0);
	if (server->io_descriptor == -1) {
		perror("epoll_create1");
		abort();
	}
#elif defined(KQUEUE_MODE)
	server->io_descriptor = kqueue();
	if (server->io_descriptor == -1) {
		perror("kqueue");
		abort();
	}
#else
	server->io_descriptor = -1;
#endif
	
	return server;
}

void
chat_server_delete(struct chat_server *server)
{
	if (server->socket >= 0)
		close(server->socket);
	
	for (int i = 0; i < server->peer_count; i++) {
		struct chat_peer *peer = &server->peers[i];
		if (peer->is_active) {
			chat_server_remove_peer(server, peer);
		} else {
			chat_buffer_free(&peer->input_buffer);
			chat_buffer_free(&peer->output_buffer);
#if NEED_AUTHOR
			free(peer->name);
#endif
		}
	}
	
	free(server->peers);
	chat_message_queue_free(&server->message_queue);
	
#if NEED_SERVER_FEED
	chat_buffer_free(&server->server_buffer);
#endif
	
	if (server->io_descriptor >= 0)
		close(server->io_descriptor);
	
	free(server);
}

int
chat_server_listen(struct chat_server *server, uint16_t port)
{
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	
	server->socket = socket(AF_INET, SOCK_STREAM, 0);
	if (server->socket < 0) {
		perror("socket");
		return -1;
	}
	
	if (set_nonblocking(server->socket) < 0) {
		perror("fcntl");
		close(server->socket);
		server->socket = -1;
		return -1;
	}
	
	int opt = 1;
	if (setsockopt(server->socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
		perror("setsockopt");
		close(server->socket);
		server->socket = -1;
		return -1;
	}
	
	if (bind(server->socket, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("bind");
		close(server->socket);
		server->socket = -1;
		return -1;
	}
	
	if (listen(server->socket, SOMAXCONN) < 0) {
		perror("listen");
		close(server->socket);
		server->socket = -1;
		return -1;
	}
	
#ifdef EPOLL_MODE
	struct epoll_event event;
	event.events = EPOLLIN | EPOLLET;
	event.data.ptr = server;
	if (epoll_ctl(server->io_descriptor, EPOLL_CTL_ADD, server->socket, &event) < 0) {
		perror("epoll_ctl: listen");
		close(server->socket);
		server->socket = -1;
		return -1;
	}
#elif defined(KQUEUE_MODE)
	struct kevent event;
	EV_SET(&event, server->socket, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, server);
	if (kevent(server->io_descriptor, &event, 1, NULL, 0, NULL) < 0) {
		perror("kevent: listen");
		close(server->socket);
		server->socket = -1;
		return -1;
	}
#endif
	
	return 0;
}

struct chat_message *
chat_server_pop_next(struct chat_server *server)
{
	struct chat_message *message = chat_message_queue_pop(&server->message_queue);
	
	if (server->message_queue.read_pos > server->message_queue.size / 2) {
		chat_message_queue_compact(&server->message_queue);
	}
	
	return message;
}

static void
server_handle_output_for_all_clients(struct chat_server *server)
{
	bool progress = false;
	do {
		progress = false;
		for (int i = 0; i < server->peer_count; i++) {
			struct chat_peer *peer = &server->peers[i];
			if (peer->is_active && peer->output_buffer.used > peer->output_buffer.processed) {
				ssize_t before = peer->output_buffer.processed;
				chat_server_handle_client_output(peer);
				if (peer->output_buffer.processed > before) {
					progress = true;
				}
			}
		}
	} while (progress);
}

int
chat_server_update(struct chat_server *server, double timeout)
{
	if (server->socket < 0) {
		return CHAT_ERR_NOT_STARTED;
	}
	
#ifdef EPOLL_MODE
	struct epoll_event events[MAX_PEERS];
	int timeout_ms = timeout < 0 ? -1 : (int)(timeout * 1000);
	
	for (int i = 0; i < server->peer_count; i++) {
		struct chat_peer *peer = &server->peers[i];
		if (!peer->is_active)
			continue;
			
		if (peer->output_buffer.used > peer->output_buffer.processed) {
			struct epoll_event event;
			event.events = EPOLLIN | EPOLLOUT | EPOLLET;
			event.data.ptr = peer;
			epoll_ctl(server->io_descriptor, EPOLL_CTL_MOD, peer->socket, &event);
		}
	}
	
	int event_count = epoll_wait(server->io_descriptor, events, MAX_PEERS, timeout_ms);
	if (event_count < 0) {
		if (errno == EINTR) {
			return 0;
		}
		perror("epoll_wait");
		return -1;
	}
	
	if (event_count == 0) {
		return CHAT_ERR_TIMEOUT;
	}
	
	for (int i = 0; i < event_count; i++) {
		if (events[i].data.ptr == server) {
			struct sockaddr_in client_addr;
			socklen_t client_len = sizeof(client_addr);
			
			while (1) {
				int client_sock = accept(server->socket, (struct sockaddr *)&client_addr, &client_len);
				if (client_sock < 0) {
					if (errno == EAGAIN || errno == EWOULDBLOCK) {
						break;
					}
					perror("accept");
					break;
				}
				
				if (set_nonblocking(client_sock) < 0) {
					perror("fcntl");
					close(client_sock);
					continue;
				}
				
				chat_server_add_peer(server, client_sock);
			}
		} else {
			struct chat_peer *peer = events[i].data.ptr;
			
			if (!peer->is_active)
				continue;
				
			if ((events[i].events & EPOLLIN) || (events[i].events & EPOLLHUP) || (events[i].events & EPOLLERR)) {
				chat_server_handle_client_input(server, peer);
				
				if (!peer->is_active)
					continue;
			}
			
			if (events[i].events & EPOLLOUT) {
				chat_server_handle_client_output(peer);
			}
		}
	}
	
#elif defined(KQUEUE_MODE)
	struct kevent events[MAX_PEERS];
	struct timespec ts;
	struct timespec *tsp = NULL;
	
	if (timeout >= 0) {
		ts.tv_sec = (time_t)timeout;
		ts.tv_nsec = (long)((timeout - ts.tv_sec) * 1e9);
		tsp = &ts;
	}
	
	int event_count = kevent(server->io_descriptor, NULL, 0, events, MAX_PEERS, tsp);
	if (event_count < 0) {
		if (errno == EINTR) {
			return 0;
		}
		perror("kevent");
		return -1;
	}
	
	if (event_count == 0) {
		return CHAT_ERR_TIMEOUT;
	}
	
	for (int i = 0; i < event_count; i++) {
		if (events[i].udata == server) {
			struct sockaddr_in client_addr;
			socklen_t client_len = sizeof(client_addr);
			
			while (1) {
				int client_sock = accept(server->socket, (struct sockaddr *)&client_addr, &client_len);
				if (client_sock < 0) {
					if (errno == EAGAIN || errno == EWOULDBLOCK) {
						break;
					}
					perror("accept");
					break;
				}
				
				if (set_nonblocking(client_sock) < 0) {
					perror("fcntl");
					close(client_sock);
					continue;
				}
				
				chat_server_add_peer(server, client_sock);
			}
		} else {
			struct chat_peer *peer = events[i].udata;
			
			if (events[i].filter == EVFILT_READ) {
				chat_server_handle_client_input(server, peer);
			} else if (events[i].filter == EVFILT_WRITE && peer->is_active) {
				chat_server_handle_client_output(peer);
			}
		}
	}
#else
	struct pollfd *pollfds = malloc((1 + server->peer_count) * sizeof(struct pollfd));
	
	pollfds[0].fd = server->socket;
	pollfds[0].events = POLLIN;
	
	int poll_count = 1;
	for (int i = 0; i < server->peer_count; i++) {
		struct chat_peer *peer = &server->peers[i];
		if (!peer->is_active)
			continue;
		
		pollfds[poll_count].fd = peer->socket;
		pollfds[poll_count].events = POLLIN;
		
		if (peer->output_buffer.used > peer->output_buffer.processed) {
			pollfds[poll_count].events |= POLLOUT;
		}
		
		poll_count++;
	}
	
	int timeout_ms = timeout < 0 ? -1 : (int)(timeout * 1000);
	int event_count = poll(pollfds, poll_count, timeout_ms);
	
	if (event_count < 0) {
		if (errno == EINTR) {
			free(pollfds);
			return 0;
		}
		perror("poll");
		free(pollfds);
		return -1;
	}
	
	if (event_count == 0) {
		free(pollfds);
		return CHAT_ERR_TIMEOUT;
	}
	
	if (pollfds[0].revents & POLLIN) {
		struct sockaddr_in client_addr;
		socklen_t client_len = sizeof(client_addr);
		
		while (1) {
			int client_sock = accept(server->socket, (struct sockaddr *)&client_addr, &client_len);
			if (client_sock < 0) {
				if (errno != EAGAIN && errno != EWOULDBLOCK) {
					perror("accept");
				}
				break;
			}
			
			if (set_nonblocking(client_sock) < 0) {
				perror("fcntl");
				close(client_sock);
				continue;
			}
			
			chat_server_add_peer(server, client_sock);
		}
	}
	
	poll_count = 1;
	for (int i = 0; i < server->peer_count; i++) {
		struct chat_peer *peer = &server->peers[i];
		if (!peer->is_active || poll_count >= (1 + server->peer_count))
			continue;
		
		if (pollfds[poll_count].revents & (POLLIN | POLLHUP)) {
			chat_server_handle_client_input(server, peer);
		}
		
		if ((pollfds[poll_count].revents & POLLOUT) && peer->is_active) {
			chat_server_handle_client_output(peer);
		}
		
		poll_count++;
	}
	
	free(pollfds);
#endif

#if NEED_SERVER_FEED
	char *data = server->server_buffer.data;
	uint32_t start = 0;
	bool processed_message = false;
	
	for (uint32_t i = 0; i < server->server_buffer.used; i++) {
		if (data[i] == '\n') {
			struct chat_message *message = create_message(data + start, i - start, NULL);
			if (message) {
				chat_message_queue_push(&server->message_queue, message);
				
				chat_server_broadcast_message(server, message, NULL);
				processed_message = true;
			}
			start = i + 1;
		}
	}
	
	if (start > 0) {
		memmove(data, data + start, server->server_buffer.used - start);
		server->server_buffer.used -= start;
	}
	
	if (processed_message) {
		server_handle_output_for_all_clients(server);
	}
#endif

	server_handle_output_for_all_clients(server);
	
	return 0;
}

int
chat_server_get_descriptor(const struct chat_server *server)
{
#if NEED_SERVER_FEED
	return server->io_descriptor;
#else
	(void)server;
	return -1;
#endif
}

int
chat_server_get_socket(const struct chat_server *server)
{
	return server->socket;
}

int
chat_server_get_events(const struct chat_server *server)
{
	if (server->socket < 0) {
		return 0;
	}
	
	int events = CHAT_EVENT_INPUT;
	
	for (int i = 0; i < server->peer_count; i++) {
		struct chat_peer *peer = &server->peers[i];
		if (peer->is_active && peer->output_buffer.used > peer->output_buffer.processed) {
			events |= CHAT_EVENT_OUTPUT;
			break;
		}
	}
	
	return events;
}

int
chat_server_feed(struct chat_server *server, const char *msg, uint32_t msg_size)
{
#if NEED_SERVER_FEED
	chat_buffer_ensure_capacity(&server->server_buffer, msg_size);
	memcpy(server->server_buffer.data + server->server_buffer.used, msg, msg_size);
	server->server_buffer.used += msg_size;
	
	if (msg_size == 1 && msg[0] == '\n') {
		char *data = server->server_buffer.data;
		uint32_t start = 0;
		
		for (uint32_t i = 0; i < server->server_buffer.used; i++) {
			if (data[i] == '\n') {
				struct chat_message *message = create_message(data + start, i - start, NULL);
				if (message) {
					chat_message_queue_push(&server->message_queue, message);
					
					chat_server_broadcast_message(server, message, NULL);
					
					server_handle_output_for_all_clients(server);
				}
				start = i + 1;
			}
		}
		
		if (start > 0) {
			memmove(data, data + start, server->server_buffer.used - start);
			server->server_buffer.used -= start;
		}
	}
	
	return 0;
#else
	(void)server;
	(void)msg;
	(void)msg_size;
	return CHAT_ERR_NOT_IMPLEMENTED;
#endif
}
