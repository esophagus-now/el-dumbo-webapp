#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <event2/event.h>
#include <arpa/inet.h> //htons
#include <unistd.h>
#include <mm_err.h>

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

	int rc = bind(fd, (struct sockaddr *) &addr, sizeof(addr));
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

//Rather than use global variables, it's super easy to just use a
//struct full of all the things I wanted to make global. This makes
//it easier in the future if I want to do multi-threaded
typedef struct {
	struct event_base *eb;
} globals;

void echo_cb(evutil_socket_t fd, short what, void *arg) {
	globals *g = arg;

	char buf[1024];
	int rc = read(fd, buf, sizeof(buf));
	if (rc < 0) {
		perror("Could not read from socket");
		close(fd);
		event_base_loopbreak(g->eb);
	} else if (rc == 0) {
		puts("Done");
		close(fd);
		event_base_loopbreak(g->eb);
	} else {
		int rc2 = write(fd, buf, rc);
		if (rc2 < rc) {
			if (errno != 0) {
				perror("Could not write data to peer");
				close(fd);
				event_base_loopbreak(g->eb);
			} else {
				puts("This is a recoverable case, but we're stopping");
				close(fd);
				event_base_loopbreak(g->eb);
			}
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

	struct event_base *eb = event_base_new();
	g.eb = eb;
	
	struct event *acc = event_new(eb, fd, EV_READ, acc_cb, &g);
	event_add(acc, NULL);

	event_base_dispatch(eb);

	puts("Goodbye, Sailor!");
	event_base_free(eb);
	close(fd);
	return 0;
}
