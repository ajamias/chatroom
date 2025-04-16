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
#include <libpq-fe.h>


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
int store_message(char *message, char safe_ip_str[INET_ADDRSTRLEN], bool is_sent);
void update_sent_message(int message_id, char safe_ip_str[INET_ADDRSTRLEN]);
void handle_client_disconnect(uint64_t client_socket, uint32_t s_addr);
void *handle_remotes(void *arg);
void *handle_remote(void *info);

const uint8_t CONFIRMATION = 42;

char *server_bind_address;
char *client_port;
char *remote_port;

pthread_mutex_t client_creation_lock;

PGconn *conn = NULL;

int main(int argc, char **argv)
{
	int ret;

	if (argc != 5) {
		printf("Usage: server <binding_ip> <client_port> <remote_port> <psql_password>\n");
		_exit(EXIT_FAILURE);
	}

	server_bind_address = argv[1];
	client_port = argv[2];
	remote_port = argv[3];

	const char *conn_template = "host=localhost port=5432 dbname=chatroom_%s user=postgres password=%s";
	int query_length = snprintf(NULL, 0, conn_template, server_bind_address, argv[4]) + 1;
	char *conninfo = (char *) calloc(query_length, sizeof(char));
	snprintf(conninfo, query_length, conn_template, server_bind_address, argv[4]);
	conn = PQconnectdb(conninfo);
	free(conninfo);
	if (PQstatus(conn) != CONNECTION_OK) {
		fprintf(stderr, "PQconnectdb: %s", PQerrorMessage(conn));
		PQfinish(conn);
		_exit(-1);
	}

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
		close(serve_client_socket);
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
		//if (errno != EBADF)
			perror("recv");
	case 0:
		printf("server: client disconnected\n");
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
			remote_to_client_fd[remote_addr.s_addr].erase(client_socket);
			pthread_mutex_unlock(&client_creation_lock);
			pthread_exit(NULL);
		}

		remote_info->socket_fd = -1;
		remote_info->thread_id = (pthread_t) -1;

		remote_info->lock = (pthread_mutex_t *) malloc(sizeof(pthread_mutex_t));
		if (remote_info->lock == NULL) {
			perror("malloc");
			free(remote_info);
			close(client_socket);
			remote_to_client_fd[remote_addr.s_addr].erase(client_socket);
			pthread_mutex_unlock(&client_creation_lock);
			pthread_exit(NULL);
		}

		if (pthread_mutex_init(remote_info->lock, NULL) != 0) {
			perror("pthread_mutex_init");
			free(remote_info->lock);
			free(remote_info);
			close(client_socket);
			remote_to_client_fd[remote_addr.s_addr].erase(client_socket);
			pthread_mutex_unlock(&client_creation_lock);
			pthread_exit(NULL);
		}

		remote_to_remote_info[remote_addr.s_addr] = remote_info;
	}
	pthread_mutex_unlock(&client_creation_lock);

	pthread_mutex_t *client_message_lock = remote_to_remote_info[remote_addr.s_addr]->lock;

	while (1) {
		uint32_t network_message_size;
		switch (recv(client_socket, &network_message_size, sizeof(network_message_size), MSG_WAITALL)) {
		case -1:
			//if (errno != EBADF)
				perror("recv");
		case 0:
			printf("server: client disconnected\n");
			handle_client_disconnect(client_socket, remote_addr.s_addr);
			pthread_exit(NULL);
			break;
		}

		pthread_mutex_lock(client_message_lock);
		printf("server: locking in\n");

		uint32_t message_size = ntohl(network_message_size);
		printf("server: recv message length: %d\n", message_size);

		char *message = (char *) malloc(sizeof(uint32_t) + message_size + 1);
		if (message == NULL) {
			perror("malloc");
			pthread_mutex_unlock(client_message_lock);
			handle_client_disconnect(client_socket, remote_addr.s_addr);
			pthread_exit(NULL);
		}
		*(uint32_t *) message = network_message_size;
		message[message_size] = '\0';

		switch (recv(client_socket, &message[sizeof(uint32_t)], message_size, MSG_WAITALL)) {
		case -1:
			//if (errno != EBADF)
				perror("recv");
		case 0:
			printf("server: client disconnected\n");
			pthread_mutex_unlock(client_message_lock);
			handle_client_disconnect(client_socket, remote_addr.s_addr);
			free(message);
			pthread_exit(NULL);
			break;
		}
		printf("server: recv from client: %s", &message[sizeof(uint32_t)]);

		char safe_ip_str[INET_ADDRSTRLEN] = {0};
		strcpy(safe_ip_str, inet_ntoa(remote_addr));
		for (int i = 0; i < INET_ADDRSTRLEN; ++i) {
			if (safe_ip_str[i] == '.')
				safe_ip_str[i] = '_';
		}

		int remote_socket = remote_to_remote_info[remote_addr.s_addr]->socket_fd;
		bool remote_is_connected = remote_socket != -1;
		if (!remote_is_connected) {
			struct attempt_connect_info attempt_info;
			if (pthread_mutex_init(&attempt_info.lock, NULL)) {
				perror("pthread_mutex_init");
				pthread_mutex_unlock(client_message_lock);
				handle_client_disconnect(client_socket, remote_addr.s_addr);
				free(message);
				pthread_exit(NULL);
			}
			if (pthread_cond_init(&attempt_info.cond, NULL)) {
				perror("pthread_cond_init");
				pthread_mutex_destroy(&attempt_info.lock);
				pthread_mutex_unlock(client_message_lock);
				handle_client_disconnect(client_socket, remote_addr.s_addr);
				free(message);
				pthread_exit(NULL);
			}
			attempt_info.remote_is_connected = false;
			attempt_info.remote_addr = remote_addr.s_addr;

			pthread_t remote_connect_tid;
			if (pthread_create(&remote_connect_tid, NULL, attempt_remote_connect, &attempt_info) != 0) {
				perror("pthread_create");
				pthread_mutex_destroy(&attempt_info.lock);
				pthread_cond_destroy(&attempt_info.cond);
				pthread_mutex_unlock(client_message_lock);
				handle_client_disconnect(client_socket, remote_addr.s_addr);
				free(message);
				pthread_exit(NULL);
			}

			pthread_mutex_lock(&attempt_info.lock);
			pthread_cond_wait(&attempt_info.cond, &attempt_info.lock);
			pthread_mutex_unlock(&attempt_info.lock);
			pthread_mutex_destroy(&attempt_info.lock);
			pthread_cond_destroy(&attempt_info.cond);

			if (!attempt_info.remote_is_connected) {
				printf("server: remote is not connected\n");
				store_message(message, safe_ip_str, false);
			}

			remote_is_connected = attempt_info.remote_is_connected;
			remote_socket = remote_to_remote_info[remote_addr.s_addr]->socket_fd;
		}

		if (remote_is_connected) {
			// TODO: handle error cases and memory leaks
			const char *get_messages_template = "SELECT id, message FROM \"%s\" WHERE sent_at IS NULL ORDER BY created_at ASC;";
			int query_length = snprintf(NULL, 0, get_messages_template, safe_ip_str) + 1;
			char *get_messages_query = (char *) calloc(query_length, sizeof(char));
			snprintf(get_messages_query, query_length, get_messages_template, safe_ip_str);
			PGresult *res = PQexec(conn, get_messages_query);
			free(get_messages_query);
			if (PQresultStatus(res) != PGRES_TUPLES_OK) {
				fprintf(stderr, "PQexec(get_messages_query): %s\n", PQerrorMessage(conn));
				PQclear(res);
			}

			int nrows = PQntuples(res);
			uint32_t *message_ids = (uint32_t *) calloc(nrows, sizeof(uint32_t));
			if (message_ids == NULL) {
				perror("malloc");
				PQclear(res);
			}
			char **messages = (char **) calloc(nrows, sizeof(char *));
			if (messages == NULL) {
				perror("malloc");
				PQclear(res);
			}

			for (int i = 0; i < nrows; i++) {
				message_ids[i] = atoi(PQgetvalue(res, i, 0));
				const char *msg = PQgetvalue(res, i, 1);
				uint32_t message_size = strlen(msg);
				messages[i] = (char *) malloc(sizeof(uint32_t) + message_size + 1);
				*(uint32_t *)messages[i] = htonl(message_size);
				memcpy(&messages[i][sizeof(uint32_t)], msg, message_size);
				messages[i][sizeof(uint32_t) + message_size] = '\0';
			}
			PQclear(res);
			
			for (int i = 0; i < nrows; ++i) {
				printf("server: sending to %s message: %s\n", safe_ip_str, &messages[i][sizeof(uint32_t)]);
				if (send(remote_socket, messages[i], sizeof(uint32_t) + message_size, 0) != sizeof(uint32_t) + message_size) {
					perror("send");
					break;
				} else {
					update_sent_message(message_ids[i], safe_ip_str);
				}
			}
			for (int i = 0; i < nrows; ++i)
				free(messages[i]);

			free(message_ids);
			free(messages);

			if (send(remote_socket, message, sizeof(uint32_t) + message_size, 0) != sizeof(uint32_t) + message_size)
				store_message(message, safe_ip_str, false);
			else
				store_message(message, safe_ip_str, true);
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
	if (recv(remote_socket, &confirmation, sizeof(confirmation), 0) != sizeof(confirmation) || confirmation != CONFIRMATION) {
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
		case -1:
			//if (errno != EBADF)
				perror("recv");
		case 0:
			printf("server: remote disconnected\n");
			close(remote_socket);
			remote_info->socket_fd = -1;
			remote_info->thread_id = (pthread_t) -1;
			pthread_exit(NULL);
			break;
		}

		pthread_mutex_lock(remote_info->lock);
		printf("server: locking in\n");

		uint32_t message_size = ntohl(network_message_size);
		printf("server: recv message length: %d\n", message_size);

		char *message = (char *) malloc(sizeof(uint32_t) + message_size + 1);
		if (message == NULL) {
			perror("malloc");
			close(remote_socket);
			remote_info->socket_fd = -1;
			remote_info->thread_id = (pthread_t) -1;
			pthread_mutex_unlock(remote_info->lock);
			pthread_exit(NULL);
		}
		*(uint32_t *) message = network_message_size;
		message[message_size] = '\0';

		switch (recv(remote_socket, &message[sizeof(uint32_t)], message_size, MSG_WAITALL)) {
		case -1:
			//if (errno != EBADF)
				perror("recv");
		case 0:
			printf("server: remote disconnected\n");
			close(remote_socket);
			remote_info->socket_fd = -1;
			remote_info->thread_id = (pthread_t) -1;
			free(message);
			pthread_mutex_unlock(remote_info->lock);
			pthread_exit(NULL);
			break;
		}
		printf("server: recv from remote: %s", &message[sizeof(uint32_t)]);

		for (uint32_t client_fd : remote_to_client_fd[remote_info->addr.sin_addr.s_addr]) {
			send(client_fd, message, sizeof(uint32_t) + message_size, 0);
		}
		printf("server: sent to connected clients\n");

		free(message);

		pthread_mutex_unlock(remote_info->lock);
		printf("server: unlocking in\n");
	}

	return NULL;
}


int store_message(char *message, char safe_ip_str[INET_ADDRSTRLEN], bool is_sent)
{
	int message_id = -1;

	const char *create_table_template = \
	"CREATE TABLE IF NOT EXISTS \"%s\" ("
		"id SERIAL PRIMARY KEY, "
		"message TEXT NOT NULL, "
		"created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP, "
		"sent_at TIMESTAMP DEFAULT NULL"
	");";
	int query_length = snprintf(NULL, 0, create_table_template, safe_ip_str) + 1;
	char *create_table_query = (char *) calloc(query_length, sizeof(char));
	snprintf(create_table_query, query_length, create_table_template, safe_ip_str);

	PGresult *res = PQexec(conn, create_table_query);
	if (PQresultStatus(res) != PGRES_COMMAND_OK) {
		fprintf(stderr, "PQexec(create_table_query): %s", PQerrorMessage(conn));
		free(create_table_query);
		PQclear(res);
		return message_id;
	}

	free(create_table_query);
	PQclear(res);

	printf("server: I will store the %s message: %s\n", is_sent ? "sent" : "unsent", &message[sizeof(uint32_t)]);

	const char *insert_message_template = "INSERT INTO \"%s\" (message, sent_at) VALUES ($1, %s) RETURNING id;";
	query_length = snprintf(NULL, 0, insert_message_template, safe_ip_str, is_sent ? "NOW()" : "NULL") + 1;
	char *insert_message_query = (char *) calloc(query_length, sizeof(char));
	snprintf(insert_message_query, query_length, insert_message_template, safe_ip_str, is_sent ? "NOW()" : "NULL");

	const char *message_param[1] = { &message[sizeof(uint32_t)] };
	res = PQexecParams(
		conn,
		insert_message_query,
		1,
		NULL,
		message_param,
		NULL,
		NULL,
		0
	);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		fprintf(stderr, "PQexecParams(insert_message_query): %s", PQerrorMessage(conn));
	else
		message_id = atoi(PQgetvalue(res, 0, 0));

	free(insert_message_query);
	PQclear(res);

	return message_id;
}


void update_sent_message(int message_id, char safe_ip_str[INET_ADDRSTRLEN])
{
	printf("server: sent to remote recipient\n");

	const char *update_sent_template = "UPDATE \"%s\" SET sent_at = NOW() WHERE id = %d;";
	int query_length = snprintf(NULL, 0, update_sent_template, safe_ip_str, message_id) + 1;
	char *update_sent_query = (char *) calloc(query_length, sizeof(char));
	snprintf(update_sent_query, query_length, update_sent_template, safe_ip_str, message_id);
	PGresult *res = PQexec(conn, update_sent_query);
	free(update_sent_query);
	if (PQresultStatus(res) != PGRES_COMMAND_OK) {
		fprintf(stderr, "PQexec(update_sent_query): %s\n", PQerrorMessage(conn));
	}

	PQclear(res);
}


void handle_client_disconnect(uint64_t client_socket, uint32_t s_addr)
{
	close(client_socket);

	remote_to_client_fd[s_addr].erase(client_socket);
	printf("server: there are now %zu clients connected\n", remote_to_client_fd[s_addr].size());
	if (remote_to_client_fd[s_addr].size() == 0) {
		if (remote_to_remote_info[s_addr]->lock != NULL) {
			pthread_mutex_destroy(remote_to_remote_info[s_addr]->lock);
			free(remote_to_remote_info[s_addr]->lock);
		}
		printf("shutting down thread and socket %d", remote_to_remote_info[s_addr]->socket_fd);
		shutdown(remote_to_remote_info[s_addr]->socket_fd, SHUT_RDWR);
		if (pthread_join(remote_to_remote_info[s_addr]->thread_id, NULL) != 0)
			perror("pthread_join");
		free(remote_to_remote_info[s_addr]);
		remote_to_remote_info.erase(s_addr);
		remote_to_client_fd.erase(s_addr);
	}
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

		if (remote_to_remote_info.find(remote_addr.sin_addr.s_addr) != remote_to_remote_info.end()) {
			printf("server: accepted remote connection from %s\n", inet_ntoa(remote_addr.sin_addr));

			uint8_t confirmation = CONFIRMATION;
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
			printf("server: A remote connection not matching a client was attempted from %s\n", inet_ntoa(remote_addr.sin_addr));
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
		case -1:
			//if (errno != EBADF)
				perror("recv");
		case 0:
			printf("server: remote disconnected\n");
			close(remote_socket);
			remote_info->socket_fd = -1;
			remote_info->thread_id = (pthread_t) -1;
			pthread_exit(NULL);
			break;
		}

		pthread_mutex_lock(remote_info->lock);
		printf("server: locking in\n");

		uint32_t message_size = ntohl(network_message_size);
		printf("server: recv message length: %d\n", message_size);

		char *message = (char *) malloc(sizeof(uint32_t) + message_size + 1);
		if (message == NULL) {
			// TODO
		}
		*(uint32_t *) message = network_message_size;
		message[message_size] = '\0';

		switch (recv(remote_info->socket_fd, &message[sizeof(uint32_t)], message_size, 0)) {
		case -1:
			//if (errno != EBADF)
				perror("recv");
		case 0:
			printf("server: remote disconnected\n");
			close(remote_info->socket_fd);
			remote_info->socket_fd = -1;
			remote_info->thread_id = (pthread_t) -1;
			free(message);
			pthread_mutex_unlock(remote_info->lock);
			pthread_exit(NULL);
			break;
		}
		printf("server: recv from remote: %s", &message[sizeof(uint32_t)]);

		for (uint32_t client_fd : remote_to_client_fd[remote_info->addr.sin_addr.s_addr]) {
			send(client_fd, message, sizeof(uint32_t) + message_size, 0);
		}
		printf("server: sent to connected clients\n");

		free(message);

		pthread_mutex_unlock(remote_info->lock);
		printf("server: unlocking in\n");
	}

	return NULL;
}
