#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/errno.h>

#define PORT 10000


int main(int argc, char **argv)
{
	if (argc != 3) {
		printf("Usage: client <ip_address> <port>\n");
		_exit(-1);
	}

	int client_socket = socket(AF_INET, SOCK_STREAM, 0);
	if (client_socket == -1) {
		perror("server_socket");
		_exit(errno);
	}

	struct sockaddr_in server_addr = {
		.sin_family = AF_INET,
		.sin_port = htons(PORT)
	};
	if (inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr) != 1) {
		perror("inet_pton");
		_exit(errno);
	}

	if (connect(client_socket, server_addr, sizeof(server_addr)) == -1) {
		perror("connect");
		_exit(errno);
	}
	
	

	return 0;
}
