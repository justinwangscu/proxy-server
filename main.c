#include "server.h"

#include <unistd.h> // close()

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <stdio.h>
#include <stdlib.h>

#include <signal.h>
#include <errno.h>

#include <pthread.h>

#define SERVER_BACKLOG  100		    // Number of connections allowed 
#define THREAD_POOL_LEN 1		    // Number of threads allowed 


#define SERVER_PORT		3003		// Port number of server
#define CLIENT		    80		    // Port number of client

// Global Variables
int volatile    running = 1;
int             client_socket = 0;              // socket descriptor
pthread_t       thread_pool[THREAD_POOL_LEN];   // queue of threads
                                                // threads are joined after each new connection after the pool is filled
size_t          connection_count = 0;
int volatile    threads_used = 0;
int volatile    thread_index = 0;
int volatile    queue_index = 0;

static void sigintHandler(int sig) {
    write(STDERR_FILENO, "Caught SIGINT\n", 15);
    running = 0;
}

int main() {
    // Socket descriptors
    int         running = 1;
    int        client_connection = 0;           // new connection descriptor

	struct     sockaddr_in client_addr;         // Address format structure
	// struct hostent *hostentry;

	int         addrlen = sizeof(client_addr);  // size of sockaddr_in structure

    if(signal(SIGINT, sigintHandler) == SIG_ERR) {
        do {
            perror("signal SIGINT");
            exit(EXIT_FAILURE);
        } while(0);
    }

    sigaction(SIGPIPE, &(struct sigaction){SIG_IGN}, NULL);

	// Create socket for connecting to client.
    if ((client_socket = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Error: Could not create socket! \n");
        return -1;
    }
    
    // To prevent "Address in use" error
    // The SO_REUSEADDR socket option, explicitly allows a process to bind to a port which remains in TIME_WAIT
    if (setsockopt(client_socket, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) < 0)
        perror("setsockopt(SO_REUSEADDR) failed!");
	

	// bind socket to server address and port
    // Server Address Initialization
    int port = SERVER_PORT;
    client_addr.sin_family = AF_INET;                       // AF_INET since we expect IPv4
    client_addr.sin_addr.s_addr = inet_addr("127.0.0.1");   // INADDR_ANY -> means any address can connect
    client_addr.sin_port = htons(port);                     // sets up the port

	// Bind to socket
    if(bind(client_socket, (struct sockaddr*)&client_addr, sizeof(client_addr)) < 0) {
        perror("Bind failed!");
        return -1;
    }
    
    // Accept up to 100 connections
    if (listen(client_socket, SERVER_BACKLOG) < 0) {
        perror("Listen failed!");
        exit(EXIT_FAILURE);
    }

	
    printf("Listening...\n");

	while(running) {

		/* --------------- Accept incoming connections -------------------- */
		client_connection = accept(client_socket, (struct sockaddr*)&client_addr, (socklen_t*)&addrlen);
		if(client_connection < 0) {
			perror("Connection not accepted");
		}

		// printf("Client connection: Accepted! client_connection: %d\n\n", client_connection);

		pthread_t t;

        int *pclient = malloc(sizeof(int));
        if(pclient == NULL) {
            running = 0;
            break;
        }
        *pclient = client_connection;

        // handle connection in new thread
        pthread_create(&t, NULL, handle_connection, pclient);
        thread_pool[queue_index] = t;
        queue_index = (queue_index + 1) % THREAD_POOL_LEN;


        ++connection_count;
        if(connection_count >= THREAD_POOL_LEN) {
            // printf("\nJoining thread %d\n", queue_index);
            pthread_join(thread_pool[queue_index], NULL);
        }

	}

    printf("\n%lu connections serviced\n", connection_count);
    
    threads_used = (connection_count >= THREAD_POOL_LEN) ? THREAD_POOL_LEN 
                : connection_count;

    for(int i = 0; i < threads_used; i++) {
        printf("Joined thread %d; ", i);
        pthread_join(thread_pool[i], NULL);
    }
    printf("\n\n");
    close (client_socket);
    
    
	return 0;
}