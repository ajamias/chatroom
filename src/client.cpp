#include <string>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/errno.h>


void *handle_server(void *arg)
{
	uint64_t server_socket = (uint64_t) arg;

	while (1) {
		uint32_t message_len;
		int recv_len = recv(server_socket, &message_len, sizeof(message_len), MSG_WAITALL);
		if (recv_len == -1) {
			perror("recv");
			_exit(errno);
		}

		message_len = ntohl(message_len);

		char *message = (char *) malloc(message_len + 1);
		message[message_len] = '\0';

		recv_len = recv(server_socket, message, message_len, MSG_WAITALL);

		printf("%s", message);

		free(message);
	}
}

int main(int argc, char **argv)
{
	if (argc != 4) {
		printf("Usage: client <server_ipv4> <server_port> <remote_ipv4>\n");
		_exit(-1);
	}

	uint64_t server_socket = socket(AF_INET, SOCK_STREAM, 0);
	if (server_socket == -1ull) {
		perror("socket");
		_exit(errno);
	}

	struct sockaddr_in server_addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = inet_addr(argv[1]),
		.sin_port = htons(atoi(argv[2]))
	};

	if (connect(server_socket, (struct sockaddr *) &server_addr, sizeof(server_addr)) == -1) {
		perror("connect");
		_exit(errno);
	}
	
	pthread_t handle_server_tid;
	if (pthread_create(&handle_server_tid, NULL, handle_server, (void *) server_socket) != 0) {
		perror("pthread_create");
		_exit(errno);
	}

	struct in_addr remote_addr = {
		.s_addr = inet_addr(argv[3])
	};
	if (send(server_socket, &remote_addr.s_addr, sizeof(remote_addr.s_addr), 0) != sizeof(remote_addr.s_addr)) {
		perror("send");
		_exit(errno);
	}

	std::string message;
	while (1) {
		char c;
		switch (read(STDIN_FILENO, &c, sizeof(c))) {
		case 1:
			message.push_back(c);

			if (c == '\n') {
				uint32_t message_size = htonl(message.size());
				send(server_socket, &message_size, sizeof(message_size), 0);
				send(server_socket, message.c_str(), message.size(), 0);
				message.clear();
			}
			break;
		case 0:
			_exit(0);
			break;
		default:
			perror("read");
			break;
		}
	}

	return 0;
}
