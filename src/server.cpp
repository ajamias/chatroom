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


struct serve_info {
	int socket_fd;
	pthread_t thread_id;
	pthread_mutex_t *lock;
	struct sockaddr_in addr;
	socklen_t addr_len;
};

struct attempt_connect_info {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	bool remote_is_connected;
	uint32_t remote_addr;
};

std::map< uint32_t, std::set<uint64_t> > remote_to_client_fd;		// mapping will store client_fd iff client is connected
std::map< uint32_t, struct serve_info * > remote_to_remote_info;	// mapping will store info iff remote is connected

void *handle_clients(void *arg);
void *handle_client(void *info);
void *attempt_remote_connect(void *info);
void *handle_remotes(void *arg);
void *handle_remote(void *info);

char *server_bind_address;
char *client_port;
char *remote_port;

pthread_mutex_t client_creation_lock;

int main(int argc, char **argv)
{
	int ret;

	if (argc != 4) {
		printf("Usage: server <binding_ip> <client_port> <remote_port>\n");
		_exit(EXIT_FAILURE);
	}

	server_bind_address = argv[1];
	client_port = argv[2];
	remote_port = argv[3];

	pthread_mutex_init(&client_creation_lock, NULL);

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
		.sin_addr.s_addr = inet_addr(server_bind_address),
		.sin_port = htons(atoi(client_port))
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
	printf("server: listening for client connections\n");

	while (1) {
		uint64_t client_socket = accept(serve_client_socket, NULL, NULL);
		if (client_socket == -1ull) {
			perror("accept");
			continue;
		}
		printf("server: accepted client connection\n");

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

	pthread_mutex_lock(&client_creation_lock);
	if (remote_to_remote_info.find(remote_addr.s_addr) == remote_to_remote_info.end()) {
		struct serve_info *remote_info = (struct serve_info *) malloc(sizeof(struct serve_info));
		if (remote_info == NULL) {
			perror("malloc");
			close(client_socket);
			pthread_exit(NULL);
		}

		remote_info->socket_fd = -1;
		remote_info->thread_id = (pthread_t) -1;

		remote_info->lock = (pthread_mutex_t *) malloc(sizeof(pthread_mutex_t));
		if (remote_info->lock == NULL) {
			perror("malloc");
			close(client_socket);
			pthread_exit(NULL);
		}
		pthread_mutex_init(remote_info->lock, NULL);

		remote_to_remote_info[remote_addr.s_addr] = remote_info;
	}
	pthread_mutex_unlock(&client_creation_lock);

	pthread_mutex_t *client_message_lock = remote_to_remote_info[remote_addr.s_addr]->lock;

	while (1) {
		uint32_t network_message_size;
		switch (recv(client_socket, &network_message_size, sizeof(network_message_size), MSG_WAITALL)) {
		case 0:
			printf("server: client disconnected\n");
			close(client_socket);
			remote_to_client_fd[remote_addr.s_addr].erase(client_socket);
			pthread_exit(NULL);
			break;
		case -1:
			perror("recv");
			continue;
		}

		pthread_mutex_lock(client_message_lock);
		printf("server: locking in\n");

		uint32_t message_size = ntohl(network_message_size);
		printf("server: recv message length: %d\n", message_size);

		char *message = (char *) malloc(message_size + 1);
		message[message_size] = '\0';

		switch (recv(client_socket, message, message_size, MSG_WAITALL)) {
		case 0:
			printf("server: client disconnected\n");
			close(client_socket);
			remote_to_client_fd[remote_addr.s_addr].erase(client_socket);
			free(message);
			pthread_exit(NULL);
			break;
		case -1:
			perror("recv");
			continue;
		}
		printf("server: recv from client: %s", message);

		bool remote_is_connected = remote_to_remote_info[remote_addr.s_addr]->socket_fd != -1;
		if (!remote_is_connected) {
			struct attempt_connect_info attempt_info;
			pthread_mutex_init(&attempt_info.lock, NULL);
			pthread_cond_init(&attempt_info.cond, NULL);
			attempt_info.remote_is_connected = false;
			attempt_info.remote_addr = remote_addr.s_addr;

			pthread_t remote_connect_tid;
			pthread_create(&remote_connect_tid, NULL, attempt_remote_connect, &attempt_info);

			pthread_mutex_lock(&attempt_info.lock);
			pthread_cond_wait(&attempt_info.cond, &attempt_info.lock);
			pthread_mutex_unlock(&attempt_info.lock);
			pthread_mutex_destroy(&attempt_info.lock);

			if (!attempt_info.remote_is_connected) {
				// if connection fail, store messages for later and create a pthread that accepts connections from that IP
				
				printf("server: I will store the message: %s\n", message);
			}
		}

		if (remote_is_connected) {
			int remote_socket = remote_to_remote_info[remote_addr.s_addr]->socket_fd;
			if (send(remote_socket, &network_message_size, sizeof(network_message_size), 0) != sizeof(network_message_size)) {
				perror("send");
			}

			if (send(remote_socket, message, message_size, 0) != message_size) {
				perror("send");
			}
			printf("server: sent to remote recipient\n");
		}

		pthread_mutex_unlock(client_message_lock);
		printf("server: unlocking in\n");

		free(message);
	}

	return NULL;
}


void *attempt_remote_connect(void *info)
{
	struct attempt_connect_info *attempt_info = (struct attempt_connect_info *) info;

	uint64_t remote_socket = socket(AF_INET, SOCK_STREAM, 0);
	if (remote_socket == -1ull) {
		perror("socket");
		pthread_cond_signal(&attempt_info->cond);
		pthread_exit(NULL);
	}

	struct sockaddr_in local_addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = inet_addr(server_bind_address),
		.sin_port = 0
	};

	if (bind(remote_socket, (struct sockaddr *) &local_addr, sizeof(local_addr)) == -1) {
		perror("bind");
		pthread_cond_signal(&attempt_info->cond);
		close(remote_socket);
		pthread_exit(NULL);
	}

	struct sockaddr_in remote_addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = attempt_info->remote_addr,
		.sin_port = htons(atoi(remote_port))
	};

	if (connect(remote_socket, (struct sockaddr *) &remote_addr, sizeof(remote_addr)) == -1) {
		perror("connect");
		pthread_cond_signal(&attempt_info->cond);
		close(remote_socket);
		pthread_exit(NULL);
	}

	uint8_t confirmation = 0;
	if (recv(remote_socket, &confirmation, sizeof(confirmation), 0) != sizeof(confirmation) || confirmation != 42) {
		pthread_cond_signal(&attempt_info->cond);
		close(remote_socket);
		pthread_exit(NULL);
	}

	attempt_info->remote_is_connected = true;

	struct serve_info *remote_info = remote_to_remote_info[remote_addr.sin_addr.s_addr];
	remote_info->socket_fd = remote_socket;
	remote_info->thread_id = pthread_self();
	remote_info->addr = remote_addr;
	remote_info->addr_len = sizeof(struct sockaddr_in);

	pthread_cond_signal(&attempt_info->cond);

	while (1) {
		uint32_t network_message_size;
		switch (recv(remote_socket, &network_message_size, sizeof(network_message_size), 0)) {
		case 0:
			printf("server: remote disconnected\n");
			close(remote_socket);
			remote_info->socket_fd = -1;
			remote_info->thread_id = (pthread_t) -1;
			pthread_exit(NULL);
			break;
		case -1:
			perror("recv");
			continue;
		}

		pthread_mutex_lock(remote_info->lock);
		printf("server: locking in\n");

		uint32_t message_size = ntohl(network_message_size);
		printf("server: recv message length: %d\n", message_size);

		char *message = (char *) malloc(message_size + 1);
		message[message_size] = '\0';

		switch (recv(remote_info->socket_fd, message, message_size, 0)) {
		case 0:
			printf("server: remote disconnected\n");
			close(remote_info->socket_fd);
			remote_info->socket_fd = -1;
			remote_info->thread_id = (pthread_t) -1;
			free(message);
			pthread_mutex_unlock(remote_info->lock);
			pthread_exit(NULL);
			break;
		case -1:
			perror("recv");
			continue;
		}
		printf("server: recv from remote: %s", message);

		for (uint32_t client_fd : remote_to_client_fd[remote_info->addr.sin_addr.s_addr]) {
			send(client_fd, &network_message_size, sizeof(network_message_size), 0);
			send(client_fd, message, message_size, 0);
		}
		printf("server: sent to connected clients\n");

		free(message);

		pthread_mutex_unlock(remote_info->lock);
		printf("server: unlocking in\n");
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
		.sin_addr.s_addr = inet_addr(server_bind_address),
		.sin_port = htons(atoi(remote_port))
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
	printf("server: listening for remote connections\n");

	while (1) {
		struct sockaddr_in remote_addr;
		socklen_t remote_addr_len = sizeof(remote_addr);

		uint64_t remote_socket = accept(serve_remote_socket, (struct sockaddr *) &remote_addr, &remote_addr_len);
		if (remote_socket == -1ull) {
			perror("accept");
			continue;
		}

		// TODO: print out IPs and error check
		if (remote_to_remote_info.find(remote_addr.sin_addr.s_addr) != remote_to_remote_info.end()) {
			printf("server: accepted remote connection from %s\n", inet_ntoa(remote_addr.sin_addr));

			uint8_t confirmation = 42;
			if (send(remote_socket, &confirmation, sizeof(confirmation), 0) != sizeof(confirmation)) {
				close(remote_socket);
				continue;
			}

			struct serve_info *remote_info = remote_to_remote_info[remote_addr.sin_addr.s_addr];
			remote_info->socket_fd = remote_socket;
			remote_info->addr = remote_addr;
			remote_info->addr_len = remote_addr_len;

			pthread_t remote_thread_id;
			if (pthread_create(&remote_thread_id, NULL, handle_remote, (void *) remote_info) != 0) {
				perror("pthread_create");
				close(remote_socket);
			}
		} else {
			// reject connection
			printf("server: A remote connection not matching a client was attemptedi from %s\n", inet_ntoa(remote_addr.sin_addr));
			close(remote_socket);
		}
	}

	return NULL;
}


void *handle_remote(void *info)
{
	struct serve_info *remote_info = (struct serve_info *) info;
	remote_info->thread_id = pthread_self();
	int remote_socket = remote_info->socket_fd;
	
	while (1) {
		uint32_t network_message_size;
		switch (recv(remote_socket, &network_message_size, sizeof(network_message_size), 0)) {
		case 0:
			printf("server: remote disconnected\n");
			close(remote_socket);
			remote_info->socket_fd = -1;
			remote_info->thread_id = (pthread_t) -1;
			pthread_exit(NULL);
			break;
		case -1:
			perror("recv");
			continue;
		}

		pthread_mutex_lock(remote_info->lock);
		printf("server: locking in\n");

		uint32_t message_size = ntohl(network_message_size);
		printf("server: recv message length: %d\n", message_size);

		char *message = (char *) malloc(message_size + 1);
		message[message_size] = '\0';

		switch (recv(remote_info->socket_fd, message, message_size, 0)) {
		case 0:
			printf("server: remote disconnected\n");
			close(remote_info->socket_fd);
			remote_info->socket_fd = -1;
			remote_info->thread_id = (pthread_t) -1;
			free(message);
			pthread_mutex_unlock(remote_info->lock);
			pthread_exit(NULL);
			break;
		case -1:
			perror("recv");
			continue;
		}
		printf("server: recv from remote: %s", message);

		for (uint32_t client_fd : remote_to_client_fd[remote_info->addr.sin_addr.s_addr]) {
			send(client_fd, &network_message_size, sizeof(network_message_size), 0);
			send(client_fd, message, message_size, 0);
		}
		printf("server: sent to connected clients\n");

		free(message);

		pthread_mutex_unlock(remote_info->lock);
		printf("server: unlocking in\n");
	}

	return NULL;
}
