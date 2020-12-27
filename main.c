#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <event2/event.h>
#include <arpa/inet.h> //htons
#include <unistd.h>
#include <mm_err.h>
#include <map.h>
#include <http_parse.h>
#include <websock.h>

int get_listening_socket(int port, mm_err *err) {
	if (*err != MM_SUCCESS) return -1;

	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(port)
	};
	
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		*err = strerror(errno);
		return -1;
	}

	int rc = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (int[1]){1}, sizeof(int));
	if (rc < 0) {
		*err = strerror(errno);
		close(fd);
		return -1;
	}

	rc = bind(fd, (struct sockaddr *) &addr, sizeof(addr));
	if (rc < 0) {
		*err = strerror(errno);
		close(fd);
		return -1;
	}

	rc = listen(fd, 10);
	if (rc < 0) {
		*err = strerror(errno);
		close(fd);
		return -1;
	}

	return fd;
}


typedef struct {
	int is_http;
	union {
		http_req http;
		websock_pkt ws;
	};
} conn_state;

//Rather than use global variables, it's super easy to just use a
//struct full of all the things I wanted to make global. This makes
//it easier in the future if I want to do multi-threaded
typedef struct {
	struct event_base *eb;
	map *fd_to_state;

	mm_err *err;
} globals;

void echo_cb(evutil_socket_t fd, short what, void *arg) {
	globals *g = arg;

	char buf[1024];
	int rc = read(fd, buf, sizeof(buf));
	if (rc < 0) {
		perror("Could not read from socket");
		close(fd);
		event_base_loopbreak(g->eb);
		return;
	} else if (rc == 0) {
		puts("Done");
		close(fd);
		event_base_loopbreak(g->eb);
		return;
	}
	
	conn_state *state = map_search(g->fd_to_state, &fd);
	if (!state) {
		fprintf(stderr, "Error, could not retrieve connection state\n");
		close(fd);
		event_base_loopbreak(g->eb);
		return;
	}

	if (state->is_http) rc = write_to_http_parser(&state->http, buf, rc, g->err);
	else rc = write_to_websock_parser(&state->ws, buf, rc, g->err);

	if (*g->err != MM_SUCCESS) {
		fprintf(stderr, "Could not parse network data: %s\n", *g->err);
		close(fd);
		event_base_loopbreak(g->eb);
		return;
	}

	if (state->is_http) {
		puts("Parsed an HTTP request");
		if (is_websock_request(&state->http, g->err)) {
			puts("Switching to websocket protocol");
			char *resp = websock_handshake_response(&state->http, NULL, g->err);
			int len = strlen(resp); //TODO: websock_handshake_response should return this

			//TODO: instead of writing right away, this should all be done with the 
			//proper non-bocking events setup.
			int rc = write(fd, resp, len);
			if (rc < 0) {
				perror("Could not write websock response");
				event_base_loopbreak(g->eb);
				return;
			}
			http_req_deinit(&state->http);
			websock_pkt_init(&state->ws, g->err);
			if (*g->err != MM_SUCCESS) {
				fprintf(stderr, "Could not initialize websocket state: %s\n", *g->err);
				close(fd);
				event_base_loopbreak(g->eb);
			}
			state->is_http = 0;
		}
	} else {
		puts("Received a websockets message.");
		if (state->ws.type == WEBSOCK_CLOSE) {
			//Answer with a close frame
			char close_pkt[WEBSOCK_MAX_HDR_SIZE];
			int len = construct_websock_hdr(close_pkt, WEBSOCK_CLOSE, 1, 0, g->err);
			if (*g->err == MM_SUCCESS) {
				//TODO: enqueue instead of just writing here
				write(fd, close_pkt, len);
				close(fd);
				puts("All done with websockets");
				event_base_loopbreak(g->eb);
			}
		} else {
			puts("Echoing...");
			int payload_len = state->ws.payload_len;
			char *response = malloc(WEBSOCK_MAX_HDR_SIZE + payload_len);
			int pos = construct_websock_hdr(response, WEBSOCK_TEXT, 1, payload_len, g->err);
			if (*g->err != MM_SUCCESS) {
				fprintf(stderr, "Could not construct response: %s\n", *g->err);
				close(fd);
				event_base_loopbreak(g->eb);
				free(response);
				return;
			}

			memcpy(response + pos, state->ws.payload, payload_len);

			//TODO: enqueue to another nonblocking write event
			int rc = write(fd, response, pos + payload_len);
			if (rc < 0) {
				perror("Could not write response");
				close(fd);
				event_base_loopbreak(g->eb);
			}

			free(response);
		}
	}
}

void acc_cb(evutil_socket_t fd, short what, void *arg) {
	globals *g = arg;
	puts("Accepting a connection");

	int con_fd = accept(fd, NULL, NULL);

	if (con_fd < 0) {
		perror("Could not accept");
		event_base_loopbreak(g->eb);
		return;
	}
	
	conn_state state;
	state.is_http = 1;
	http_req_init(&state.http, g->err);

	if (*g->err != MM_SUCCESS) {
		fprintf(stderr, "Could not initialize HTTP state: %s\n", *g->err);
		close(con_fd);
		event_base_loopbreak(g->eb);
		return;
	}

	map_insert(g->fd_to_state, &con_fd, 0, &state, 0);

	struct event *echo_ev = event_new(g->eb, con_fd, EV_READ | EV_PERSIST, echo_cb, g);
	event_add(echo_ev, NULL);
}

int main() {
	mm_err err = MM_SUCCESS;
	int fd = get_listening_socket(4567, &err);
	if (err != MM_SUCCESS) {
		fprintf(stderr, "Could not open listening socket on port 4567: %s\n", err);
		return -1;
	}

	globals g;
	
	map fd_to_state;
	map_init(&fd_to_state, int, conn_state, VAL2VAL);

	g.fd_to_state = &fd_to_state;

	struct event_base *eb = event_base_new();
	g.eb = eb;
	
	struct event *acc = event_new(eb, fd, EV_READ, acc_cb, &g);
	event_add(acc, NULL);

	g.err = &err;

	event_base_dispatch(eb);
	
	if (err != MM_SUCCESS) {
		fprintf(stderr, "Failed with error %s\n", err);
	} else {
		puts("Goodbye, Sailor!");
	}

	map_free(&fd_to_state);
	event_base_free(eb);
	close(fd);
	return 0;
}
