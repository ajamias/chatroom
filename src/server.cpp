#include <string>
#include <set>
#include <map>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/errno.h>

#define CLIENT_RECV_PORT 10000
#define REMOTE_SEND_PORT 10001


struct serve_info {
	int socket_fd;
	pthread_t thread_id;
	pthread_mutex_t lock;
	struct sockaddr_in addr;
	socklen_t addr_len;
};

std::map< uint32_t, std::set<uint64_t> > remote_to_client_fd;
std::map< uint32_t, struct serve_info * > remote_to_remote_info;

void *handle_clients(void *arg);
void *handle_client(void *info);
void *handle_remotes(void *arg);
void *handle_remote(void *info);

int main() // can add int argc, char **argv later to specify port numbers
{
	int ret;

	pthread_t clients_handler_tid;
	if ((ret = pthread_create(&clients_handler_tid, NULL, handle_clients, NULL)) != 0) {
		perror("pthread_create");
		_exit(ret);
	}
	pthread_detach(clients_handler_tid);

	pthread_t remotes_handler_tid;
	if ((ret = pthread_create(&remotes_handler_tid, NULL, handle_remotes, NULL)) != 0) {
		perror("pthread_create");
		_exit(ret);
	}
	pthread_detach(remotes_handler_tid);

	// block main forever
	pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
	pthread_mutex_lock(&lock);
	pthread_cond_wait(&cond, &lock);

	return 0;
}

void *handle_clients(void *arg)
{
	(void) arg;

	int serve_client_socket = socket(AF_INET, SOCK_STREAM, 0);
	if (serve_client_socket == -1) {
		perror("socket");
		_exit(errno);
	}
	int opt = 1;
	if (setsockopt(serve_client_socket, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) == -1) {
		perror("setsockopt");
		close(serve_client_socket);
		_exit(errno);
	}

	struct sockaddr_in server_addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = INADDR_ANY,
		.sin_port = htons(CLIENT_RECV_PORT)
	};

	if (bind(serve_client_socket, (struct sockaddr *) &server_addr, sizeof(server_addr)) == -1) {
		perror("bind");
		close(serve_client_socket);
		_exit(errno);
	}

	if (listen(serve_client_socket, 8) == -1) {
		perror("listen");
		_exit(errno);
	}

	printf("server: listening on socket\n");

	while (1) {
		uint64_t client_socket = accept(serve_client_socket, NULL, NULL);
		if (client_socket == -1ull) {
			perror("accept");
			continue;
		}

		printf("server: accepted connection\n");

		pthread_t client_thread_id;
		if (pthread_create(&client_thread_id, NULL, handle_client, (void *) client_socket) != 0) {
			perror("pthread_create");
			close(client_socket);
		}
	}

	return NULL;
}


void *handle_client(void *info)
{
	uint64_t client_socket = (uint64_t) info;
	// if a client sends a message, get a mutex, and send message

	struct in_addr remote_addr;
	switch (recv(client_socket, &remote_addr.s_addr, sizeof(remote_addr.s_addr), MSG_WAITALL)) {
	case -1:
		perror("recv");
	case 0:
		close(client_socket);
		pthread_exit(NULL);
		break;
	}

	printf("server: recv target remote IP\n");

	remote_to_client_fd[remote_addr.s_addr].insert(client_socket);

	while (1) {
		uint32_t network_message_size;
		int recv_len = recv(client_socket, &network_message_size, sizeof(network_message_size), MSG_WAITALL);
		if (recv_len == -1) {
			perror("recv");
			continue;
		} else if (recv_len == 0) {
			printf("server: closing connection\n");
			close(client_socket);
			remote_to_client_fd[remote_addr.s_addr].erase(client_socket);
			pthread_exit(NULL);
		}

		uint32_t message_size = ntohl(network_message_size);

		printf("server: recv message length: %d\n", message_size);

		pthread_mutex_lock(&remote_to_remote_info[remote_addr.s_addr]->lock);

		printf("server: locking in\n");

		char *message = (char *) malloc(message_size + 1);
		message[message_size] = '\0';

		recv_len = recv(client_socket, message, message_size, MSG_WAITALL);
		printf("server: len: %d\n", recv_len);
		printf("server recv from client: %s", message);

		if (send(remote_to_remote_info[remote_addr.s_addr]->socket_fd, &network_message_size, sizeof(network_message_size), 0) != sizeof(network_message_size)) {
			perror("send");
		}

		if (send(remote_to_remote_info[remote_addr.s_addr]->socket_fd, message, message_size, 0) != message_size) {
			perror("send");
		}
		printf("server: sent to recipient\n");

		free(message);

		pthread_mutex_unlock(&remote_to_remote_info[remote_addr.s_addr]->lock);
	}

	return NULL;
}


void *handle_remotes(void *arg)
{
	(void) arg;

	int serve_remote_socket = socket(AF_INET, SOCK_STREAM, 0);
	if (serve_remote_socket == -1) {
		perror("socket");
		_exit(errno);
	}
	int opt = 1;
	if (setsockopt(serve_remote_socket, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) == -1) {
		perror("setsockopt");
		close(serve_remote_socket);
		_exit(errno);
	}

	struct sockaddr_in server_addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = INADDR_ANY,
		.sin_port = htons(REMOTE_SEND_PORT)
	};

	if (bind(serve_remote_socket, (struct sockaddr *) &server_addr, sizeof(server_addr)) == -1) {
		perror("bind");
		close(serve_remote_socket);
		_exit(errno);
	}

	if (listen(serve_remote_socket, 8) == -1) {
		perror("listen");
		_exit(errno);
	}

	pthread_t client_handler_thread_id;
	if (pthread_create(&client_handler_thread_id, NULL, handle_clients, NULL) != 0) {
		perror("pthread_create");
		_exit(errno);
	}

	while (1) {
		struct serve_info remote_info = {
			.addr_len = sizeof(struct sockaddr)
		};

		int remote_socket = accept(serve_remote_socket, (struct sockaddr *) &remote_info.addr, &remote_info.addr_len);
		if (remote_socket == -1) {
			perror("accept");
			continue;
		}

		struct serve_info *thread_arg = (struct serve_info *) malloc(sizeof(struct serve_info));
		if (thread_arg == NULL) {
			perror("malloc");
			close(remote_socket);
			continue;
		}
		memcpy(thread_arg, &remote_info, sizeof(struct serve_info));
		thread_arg->socket_fd = remote_socket;

		if (pthread_create(&thread_arg->thread_id, NULL, handle_remote, thread_arg) != 0) {
			perror("pthread_create");
			close(remote_socket);
			free(thread_arg);
		}
	}

	return NULL;
}


void *handle_remote(void *info)
{
	// for all clients "connected" to this remote address, forward message from remote to clients
/*
	struct serve_info {
		int socket_fd;
		pthread_t thread_id;
		pthread_mutex_t lock;
		struct sockaddr addr;
		socklen_t addr_len;
	}
*/

	struct serve_info *remote_info = (struct serve_info *) info;

	pthread_mutex_init(&remote_info->lock, NULL);

	// if this address already maps to a set of open client sockets, then update the map accordingly
	remote_to_client_fd.insert(make_pair(remote_info->addr.sin_addr.s_addr, std::set<uint64_t> ()));

	remote_to_remote_info[remote_info->addr.sin_addr.s_addr] = (struct serve_info *) info;

	while (1) {
		//char message[4096];
		//int recv_length = recv(remote_info->socket_fd, message, sizeof(message), 0);

		pthread_mutex_lock(&remote_info->lock);

		pthread_mutex_unlock(&remote_info->lock);
	}

	return NULL;
}
