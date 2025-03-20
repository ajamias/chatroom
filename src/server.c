#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/errno.h>

#define PORT 10000


int main(int argc, char **argv)
{
	int server_socket = socket(AF_INET, SOCK_STREAM, 0);
	if (server_socket == -1) {
		perror("socket");
		_exit(errno);
	}

	struct sockaddr_in server_addr = {
		.sin_family = AF_INET,
		.sin_addr.in_addr = INADDR_ANY,
		.sin_port = htons(PORT)
	};

	if (bind(server_socket, (struct sockaddr *) &server_addr, sizeof(server_addr)) == -1) {
		perror("bind");
		_exit(errno);
	}

	if (listen(server_socket, 8) == -1) {
		perror("listen");
		_exit(errno);
	}

	while (1) {
		
		if (accept(server_socket, NULL, NULL) == -1) {
			perror("accept");
			continue;
		}

		pthread_create(..., NULL, serve, );
	}
}
