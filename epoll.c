/* 
 * Attention:
 * To keep things simple, do not handle socket/bind/listen/.../epoll_create/epoll_wait API error 
 */
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#define DEFAULT_PORT    8080
#define MAX_CONN        16
#define MAX_EVENTS      32
#define BUF_SIZE        16
#define MAX_LINE        256

void server_run();
void client_run();

/*
 * Write all data to nonblocking socket.
 * Handle EAGAIN and EINTR properly.
 */
static void write_all(int fd, const char *buf, size_t len)
{
	size_t off = 0;

	while (off < len) {
		ssize_t n = write(fd, buf + off, len - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return; /* wait for next EPOLLOUT */
			perror("write");
			return;
		}
		off += (size_t)n;
	}
}

int main(int argc, char *argv[])
{
	int opt;
	char role = 's';

	while ((opt = getopt(argc, argv, "cs")) != -1) {
		switch (opt) {
		case 'c':
			role = 'c';
			break;
		case 's':
			break;
		default:
			printf("usage: %s [-cs]\n", argv[0]);
			exit(1);
		}
	}

	if (role == 's') {
		server_run();
	} else {
		client_run();
	}

	return 0;
}

/*
 * register events of fd to epfd
 */
static void epoll_ctl_add(int epfd, int fd, uint32_t events)
{
	struct epoll_event ev;
	ev.events = events;
	ev.data.fd = fd;
	if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) == -1) {
		perror("epoll_ctl()");
		exit(1);
	}
}

static void set_sockaddr(struct sockaddr_in *addr)
{
	memset(addr, 0, sizeof(struct sockaddr_in));
	addr->sin_family = AF_INET;
	addr->sin_addr.s_addr = INADDR_ANY;
	addr->sin_port = htons(DEFAULT_PORT);
}

static int setnonblocking(int sockfd)
{
	if (fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFL, 0) | O_NONBLOCK) ==
	    -1) {
		return -1;
	}
	return 0;
}

/*
 * epoll echo server
 */
void server_run()
{
	int i;
	int n;
	int epfd;
	int nfds;
	int listen_sock;
	int conn_sock;
	socklen_t socklen;
	char buf[BUF_SIZE];
	struct sockaddr_in srv_addr;
	struct sockaddr_in cli_addr;
	struct epoll_event events[MAX_EVENTS];

	listen_sock = socket(AF_INET, SOCK_STREAM, 0);

	set_sockaddr(&srv_addr);
	bind(listen_sock, (struct sockaddr *)&srv_addr, sizeof(srv_addr));

	setnonblocking(listen_sock);
	listen(listen_sock, MAX_CONN);

	epfd = epoll_create(1);
	epoll_ctl_add(epfd, listen_sock, EPOLLIN | EPOLLOUT | EPOLLET);

	for (;;) {
		nfds = epoll_wait(epfd, events, MAX_EVENTS, -1);
		for (i = 0; i < nfds; i++) {
			int fd = events[i].data.fd;

			if (fd == listen_sock) {
				/* handle new connection */
				socklen = sizeof(cli_addr);
				conn_sock = accept(listen_sock,
						(struct sockaddr *)&cli_addr,
						&socklen);

				if (conn_sock < 0) {
					if (errno == EAGAIN || errno == EWOULDBLOCK) {
						continue;
					}
					perror("accept");
					continue;
				}

				inet_ntop(AF_INET, &(cli_addr.sin_addr),
					  buf, sizeof(buf));
				printf("[+] connected with %s:%d\n", buf,
				       ntohs(cli_addr.sin_port));

				setnonblocking(conn_sock);
				epoll_ctl_add(epfd, conn_sock,
					      EPOLLIN | EPOLLET | EPOLLRDHUP |
					      EPOLLHUP);
			} else if (events[i].events & EPOLLIN) {
				/* handle EPOLLIN event */
				memset(buf, 0, sizeof(buf));
				n = read(fd, buf, sizeof(buf) - 1);

				if (n == 0) {
					/* connection closed by peer */
					printf("[+] connection closed\n");
					epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
					close(fd);
				} else if (n < 0) {
					/* error or EAGAIN */
					if (errno == EAGAIN || errno == EWOULDBLOCK) {
						/* no more data available in ET mode */
						continue;
					}
					perror("read");
					epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
					close(fd);
				} else {
					/* n > 0: data received */
					buf[n] = '\0';
					printf("[+] data: %s\n", buf);
					write_all(fd, buf, (size_t)n);
				}
			} else {
				printf("[+] unexpected\n");
			}

			/* check if the connection is closing */
			if (events[i].events & (EPOLLRDHUP | EPOLLHUP)) {
				printf("[+] connection closed\n");
				epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
				close(fd);
			}
		}
	}
}

/*
 * test client 
 */
void client_run()
{
	int n;
	int sockfd;
	char buf[MAX_LINE];
	struct sockaddr_in srv_addr;

	sockfd = socket(AF_INET, SOCK_STREAM, 0);

	set_sockaddr(&srv_addr);

	if (connect(sockfd, (struct sockaddr *)&srv_addr, sizeof(srv_addr)) < 0) {
		perror("connect()");
		exit(1);
	}

	for (;;) {
		printf("input: ");
		if (fgets(buf, sizeof(buf), stdin) == NULL) {
			break;
		}

		/* remove trailing newline */
		buf[strcspn(buf, "\r\n")] = '\0';

		if (buf[0] == '\0') {
			continue;
		}

		/* send data */
		n = write(sockfd, buf, strlen(buf));
		if (n < 0) {
			perror("write");
			break;
		}

		/* read echo response */
		memset(buf, 0, sizeof(buf));
		while ((n = read(sockfd, buf, sizeof(buf) - 1)) > 0) {
			buf[n] = '\0';
			printf("echo: %s\n", buf);
			memset(buf, 0, sizeof(buf));
		}

		/* handle read error */
		if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
			perror("read");
			break;
		}
	}

	close(sockfd);
}
