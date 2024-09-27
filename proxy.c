#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h> // to get file size
#include <pthread.h>

#define SOCK_READ_BUFF  8192		// Size of the buffer used to store the bytes read over socket
#define REQ_BUFF		8192		// Size of the buffer used to store requests from browser
#define RES_BUFF      	8192        // Size of the buffer used to store response to/from server
#define HOST_BUFF		64			// Buffer size for host name
#define CONLEN_BUFF		128			// Buffer size for host name

#define SERVER_BACKLOG  100		    // Number of connections allowed 
#define THREAD_POOL_LEN 1		    // Number of threads allowed 


#define SERVER_PORT		3003		// Port number of server
#define CLIENT		    80		    // Port number of client

#define MAXLINE 		10

// Global Variables
int volatile    running = 1;
int             client_socket = 0;  // socket descriptor


pthread_t       thread_pool[THREAD_POOL_LEN];   // queue of threads
                                                // threads are joined after each new connection after the pool is filled
size_t          connection_count = 0;
int volatile    threads_used = 0;
int volatile    thread_index = 0;
int volatile    queue_index = 0;

static void sigintHandler(int sig) {
    running = 0;

    
    close(client_socket);
    threads_used = (connection_count >= THREAD_POOL_LEN) ? THREAD_POOL_LEN 
                : connection_count;

    printf("closed client_socket\n");
    write(STDERR_FILENO, "Caught SIGINT\n", 15);
}


// puts host string into dest, reads from request
// INPUT: dest should be size HOST_BUFF, request is buffer holding http request
// 0 = couldn't find host string, 1 = found host string
int get_host_string(char* dest, const char *request) {
    char *startP, *endP, *colonP;
    size_t lineLen;
    char temp_buff[HOST_BUFF];
    
    // set pointer to start at "Host: " line
    startP = strstr(request, "Host: ");	

    if(startP == NULL) {
        perror("no \"Host: \" line detected");
        return 0;
    }
    
    // set start after "Host: "
    startP += strlen("Host: ");	
    // set end at end of line
    endP = strstr(startP, "\n");		// get pointer to new line

    // copy line into hostname

    lineLen = endP - startP;				// get length of line
    if (lineLen + 1 > HOST_BUFF / sizeof(char) || lineLen < 0) {
        //printf("Linelen: %lu is too long\n\n", lineLen);
        return 0;
    }
    strncpy(temp_buff, startP, lineLen);			// copy line from start of line to end into hostname
    temp_buff[lineLen] = '\0';					// add null terminator
    // printf("One line: %s\n\n", hostname);

    // set start pointer to beginning of temp
    startP = temp_buff;
    // set end pointer to end of temp 
    endP = &temp_buff[lineLen - 1];

    // if url starts with http: skip after it
    int containsHttp = strncmp(temp_buff, "http", strlen("http"));
    if(containsHttp == 0) {     // if starts with http
        startP = strstr(temp_buff, "//");
        startP += strlen("//");
        // printf("After \"//\": %s\n\n", startP);
    }

    // if url has ":<port number>" then stop it before ": " otherwise
    colonP = strstr(temp_buff, ":");
    if (colonP != NULL) {   //if it has port number
        // printf("at \":\" %s\n\n", startP);
        endP = colonP;
    }

    // recopy final string into hostname
    lineLen = endP - startP;					// get length of line
    if (lineLen + 1 > (HOST_BUFF / sizeof(char)) || lineLen < 0) {
        //printf("Linelen: %lu is too long\n\n", lineLen);
        return 0;
    }

    strncpy(dest, startP, lineLen);			// copy from start to end into hostname
    dest[lineLen] = '\0';				    // add null terminator

    return 1;
}

// Input: pointer dest is passed by reference
// Result: creates socket and connection to host
// returns 1 if successful connection, 0 if failed
int connect_to_host(const char* host, int *pserver_socket) {
    //printf("connect_to_host()\n\thost string: %s\n\n", host);

	struct addrinfo hints, *result, *rp;

	int errcode, successFlag = 0;

    int server_socket;
	
    memset (&hints, 0, sizeof(hints));

	hints.ai_family = AF_UNSPEC;		// what type of IP AF_INET = IPv4 only, AF_UNSPEC = v4 or v6
	hints.ai_socktype = SOCK_STREAM;	    // TCP SOCK_STREAM or UDP SOCK_DGRAM
	// hints.ai_flags != AI_CANONNAME;
    hints.ai_canonname = NULL;
    hints.ai_addr = NULL;
    hints.ai_next = NULL;

    /* --------------- Get Server Address Info -------------------- */
	errcode = getaddrinfo(host, "80", &hints, &result);
	if (errcode != 0) {
		fprintf(stderr, "ERROR getaddrinfo: %s\n", gai_strerror(errcode));
		return 0;
	}
    	
	// flags = NI_NUMERICHOST;
	for(rp = result; rp != NULL; rp = rp->ai_next) {
        server_socket = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);

        if (server_socket == -1) {
            perror("Error server socket");
            continue;
        }
        // rp->ai_addr->sin_port = htons(80);

        // if connection successful -> break out of loop
        if (connect(server_socket, rp->ai_addr, rp->ai_addrlen) >= 0) {
            //printf("Server connection: Accepted! server_connection\n");
            *pserver_socket = server_socket; 
            break;
        }
        
        // close socket if connection error 
        perror("Error server connect");
        close(server_socket);
	}

    //printf("Free addrinfo\n");
	freeaddrinfo(result);

    // if we reached end of loop without connection
    if(rp == NULL) {
        perror("Could not connect\n");
        return 0;
    }

    return 1;
}

// sets foundConLen to 0 if Content-Length header not found, to 1 if found
// returns contentLength if found, returns 0 if not found
size_t get_content_length(char *response, int *foundConLen) {
    char        conLenString[CONLEN_BUFF];      // buffer to hold Content-Length String

	// printf("response: \" %s\n\n", response);

	char* startP = strstr(response, "Content-Length: ");	// set pointer to start at "Host: " line
    char* endP;

    size_t conLen;

	if(startP == NULL) {
		fprintf(stderr, "no \"Content-Length: \" detected\n");
        // printf("Response: \n\n%s\n\n", response);
        *foundConLen = 0;
		return 0;
	}
	
	// set start after "Content-Length: "
	startP += strlen("Content-Length: ");	
	// printf("After \"Content-Length: \" %s\n\n", startP);

	// set end at end of line
	endP = strstr(startP, "\n");		// get pointer to new line
	// printf("After endP: %s\n\n", endP);

	// copy line into conLenString
	int lineLen = endP - startP;				// get length of line
	// printf("Subtraction: %d\n\n", lineLen);
	if (lineLen + 1 > sizeof(conLenString) / sizeof(char)) {
		fprintf(stderr, "linelen: %d is too long\n\n", lineLen);
        *foundConLen = 0;
		return 0;
	}

	strncpy(conLenString, startP, lineLen);			// copy line from start of line to end into conLenString
	conLenString[lineLen] = 0;						// add null terminator

	// printf("Final string: %s\n", conLenString);
    *foundConLen = 1;
	conLen = atoi(conLenString);
	// printf("conLen Int: %lu\n\n", conLen);

	return conLen;
}


// returns 0 if no end found
size_t bytes_to_header_end(char *response, char* response_buffer, int* foundHeaderEnd) {
    char* startP = response; 
    char* endP;

    size_t length = 0;
    size_t copy_length = 0;

    // check for CRLF
    endP = strstr(response, "\r\n\r\n");
    if(endP == NULL) {
        //printf("NO HEADER END FOUND??? Not Likely, Check code\n");
        *foundHeaderEnd = 0;
        return 0;
    }

    *foundHeaderEnd = 1;

    //printf("Found header end\n");

    //print potential header length
    length = endP - startP + strlen("\r\n\r\n");
    // printf("length to header end: %lu\n", length);
    // copy potential header string
    if(length > RES_BUFF) {
        copy_length = RES_BUFF;
    }
    else {
        copy_length = length;
    }

    strncpy(response_buffer, startP, copy_length);
    response_buffer[copy_length] = '\0';        // add null terminator
    //printf("Response Header: \n\n%s\n\n", temp);

    // return header length
    return length;
}

// returns 0 if not found, 1 if found
int chunked_encoding_check(char* response) {
    char* startP = strstr(response, "Transfer-Encoding: chunked");
    if (startP == NULL) return 0;

    // printf("This response has \"Transfer-Encoding: chunked\" and has no ContentLength\n");
    return 1;
}

// used to detect end of chunked encoding stream 
// only call after detecting response header end
// returns 0 if not found, 1 if found
int contains_end_of_stream(char *response) {
    char* startP = response; 
    char* endP = response;

    size_t length = 0;

    // check for CRLF
    endP = strstr(response, "0\r\n\r\n");
    if(endP == NULL) {
        // printf("No CRLF yet\n");
        return 0;
    }
    
    // printf("EOS detected in response: \n%s\n At endP: %s\n", startP, endP);
    return 1;
}

int send_dummy_response(int client_connection, char* res_buff) {
    size_t      soc_written = 0;                // num bytes written to a socket

    sprintf(res_buff, "HTTP/1.1 500 Internal Server Error\nContent-Type: text/plain\nContent-Length: %d\n\n", 0);
    soc_written = write(client_connection, res_buff, strlen(res_buff));
    if (soc_written < 0) {
        fprintf(stderr, "Error writing to client socket\n");
    }
    return soc_written;
}

void *handle_connection(void *pclient_connection) {
    int         client_connection = *((int *)pclient_connection);
    int         server_socket = 0;              // socket descriptor

    size_t      total_written = 0;              // total bytes written
    size_t      total_read = 0;

    size_t      soc_readed = 0;                 // num bytes read from a socket
    size_t      soc_written = 0;                // num bytes written to a socket
    size_t      content_length = 0;             // content length in defined in response header 
    size_t      content_readed = 0;             // number of bytes read after response header

	char        net_buff[SOCK_READ_BUFF];       // buffer to hold characters read from socket
	char        req_buff[REQ_BUFF];				// buffer to hold the request
	char        res_buff[RES_BUFF];      	    // buffer to hold the response header
	char        hostname[HOST_BUFF];            // buffer to hold host name

    // Free memory
    //printf("free pclient_connection\n");
    free(pclient_connection);
    pclient_connection = NULL;

    // clear buffers
    memset(net_buff, '\0', sizeof(net_buff));
    memset(req_buff, '\0', sizeof(req_buff));
    memset(res_buff, '\0', sizeof(res_buff));
    memset(hostname, '\0', sizeof(hostname));

    
    //printf("Handling Client Connection: %d\n\n", client_connection);

    /* --------------- Read incoming request ------------------------- */
    
    // read from socket
    soc_readed = read(client_connection, net_buff, sizeof(net_buff) - 1);
    if(soc_readed < 0) {
        perror("ERROR reading from socket");
        goto closeConnections;
    }
    size_t reqSize = strlen(net_buff);

    // copy request into req_buff
    strncat(req_buff, net_buff, sizeof(req_buff));

    fprintf(stderr, "Request: %s\n", req_buff);

    // // print request
    // printf("Request Size %d: \nRequest String:%.*s\n", reqSize, reqSize, req_buff);

    if(reqSize == 0) {
        soc_written = send_dummy_response(client_connection, res_buff);
        goto closeConnections;
    }

    /* --------------- Only accept GET requests -------------------- */
    
    // if(strncmp("GET", net_buff, strlen("GET")) != 0) {
    //     printf("Only GET requests accepted, sending dummy response\n");
    //     goto send_dummy_response;
    // }

    /* --------------- Get hostname string in request -------------------- */


    int foundHostName = get_host_string(hostname, net_buff);
    if (!foundHostName) {       // if we didn't get a hostname string -> send dummy response
        fprintf(stderr, "Error: Could not get host name from request\n\n");
        soc_written = send_dummy_response(client_connection, res_buff);
        goto closeConnections;
    }

    // if we sucessfully found a hostname string
    

    /* --------------- Connect to Server -------------------- */
    int connectedToHost = connect_to_host(hostname, &server_socket);
    if(!connectedToHost) {      // if we couldn't connect to server -> send dummy response
        fprintf(stderr, "connection error: %d\n", connectedToHost);
        soc_written = send_dummy_response(client_connection, res_buff);
        goto closeConnections;
    }

    /* ------------------- Forward request to server ---------------- */        
    soc_written = write(server_socket, net_buff, strlen(net_buff));
    //printf("Wrote %lu chars to server connection\n\n", soc_written);

    

    /* ------------------- Receive response from server ---------------- */
    int foundConLen = 0;            // flag = 1 when we found content-length in response header
    int foundChunkedEncoding = 0;   // flag = 1 when we found chuncked encoding in response header
    int foundHeaderEnd = 0;         // flag = 1 when we found the end of the response header

    size_t potentialConLen = 0;             // temp int for storing potential content length
    size_t potentialBytestoHeaderEnd = 0;   // temp int for storing potential bytes after header
    size_t headerSize = 0;   // header size
    size_t total_response_read = 0;         
    soc_readed = 0;
    soc_written = 0;

    int response_code_found = 0;

    int responseDone = 0;

    
    
    /* ------------------- Send Response from Server to Client -----------------*/
    //  Keep reading from server and sending to client until:
    //      if Chunked Encodeing 
    //          if we found the end of the response header
    //              -> stop when end of stream token is found
    //      if "Content-Length: " is found in HTTP response header
    //          if data delivered after header >= content-length
    //              -> stop
    do {
        memset(net_buff, 0, sizeof(net_buff));  // clear net_buff

        // read from socket
        soc_readed = read(server_socket, net_buff, sizeof(net_buff) - 1); 
        if (soc_readed < 0) {   // check read error
            perror("ERROR reading from socket\n"); // if read returns < -1 then error
            break;
        }
        if(soc_readed == 0) { // if readed is 0 -> break
            break;
        }  
        fprintf(stderr, "soc_readed: %lu\n", soc_readed);
        // printf("%lu read from server\n", soc_readed);
        total_response_read += soc_readed;

        // --------------------- Send response to client -----------------
        // send response from server
        soc_written = write(client_connection, net_buff, soc_readed);
        if (soc_written < 0) {
            perror("Error writing to client socket\n");
        }
        // printf("%lu written to client\n", soc_written);

        // Try to find response code
        if(!response_code_found) {
            // if response code is not 200 -> response done
            char *firstLine = strstr(net_buff, "\n");
            char *firstTwoHundred = strstr(net_buff, "200");

            if (firstTwoHundred == NULL || firstTwoHundred >= firstLine) {
                printf("Non-200 Response Detected\n");
                responseDone = 1;
            }
            else {
                response_code_found = 1;
            }
        }

        // Try to find "Content-Length"
        if(!foundConLen && !foundHeaderEnd) {            
            potentialConLen = get_content_length(net_buff, &foundConLen);
            if(foundConLen) {
                // update content_length
                content_length = potentialConLen;
                // printf("Content Len is: %lu\n", content_length);   
                //foundConLen = 1;
            }
        }

        // Try to find header end, look out for end of stream after header
        if(!foundHeaderEnd) { 
            potentialBytestoHeaderEnd = bytes_to_header_end(net_buff, res_buff, &foundHeaderEnd);
            // if we just found header end  
            //  -> check for end of stream marker after header
            //  if end of stream is present after header
            //      -> break
            //  update content_readed
            //  continue
            if(foundHeaderEnd) {
                // if end of stream in same buffer as header end -> end response
                char *afterHeader = strstr(net_buff, "\r\n\r\n");
                afterHeader += strlen("\r\n\r\n");
                if(contains_end_of_stream(afterHeader)) {
                    //printf("Warning: end of stream token in same buffer as header end. Small file or 404?\n");
                    responseDone = 1;
                    break;
                }


                // content_readed is size of the message read - part of the message that is the header
                content_readed = soc_readed - potentialBytestoHeaderEnd;
                headerSize += potentialBytestoHeaderEnd;

                // if content length hit -> break
                if(foundConLen) {
                    // if we read all the content we need to -> last loop
                    if(content_readed >= content_length) {
                        responseDone = 1;
                        break;
                    }
                }
                
                // we continue since we don't want to double count content_readed nor do we want to end if EOS is present in net_buff
                continue;
            }
            else {
                headerSize += soc_readed;
            }
        }

        // If we didn't find Con Len
        //  -> look out for end of stream token
        if (!foundConLen && foundHeaderEnd) {
            if(contains_end_of_stream(net_buff)) {
                responseDone = 1;
                break;
            }
        }
        // If Content-Length 
        //  -> keep track of bytes after header
        else if(foundConLen){
            
            // if we found the header -> update content_readed
            if(foundHeaderEnd) {
                content_readed += soc_readed;
                // printf("hi %lu\n", content_readed);
            }

            // printf("contentreaded: %lu\n", content_readed);

            // if we read all the content we need to -> last loop
            if(content_readed >= content_length) {
                responseDone = 1;
            }

        }
         
    } while(!responseDone);
    

    // printf("Response Total Length: %lu\n", total_response_read);
    // printf("Content Read: %lu\n\n", content_readed);

    if(foundConLen) {
        // printf("Content Length: %lu\n", content_length);
    }
    
    printf("Request");
    if(foundConLen) {
        printf(" (Content-Length: %lu):\n", content_length);
    }
    else {
        printf(":\n");
    }
    
    printf("%s\n", req_buff);

    printf("Response (Content Read: %lu):\n", content_readed - 3);

    printf("%s\n\n", res_buff);
    
    
    // close connections
    closeConnections:
    close(server_socket);
    shutdown (client_connection, SHUT_RDWR);
    close (client_connection);


    return NULL;
}


int main() {
    // Socket descriptors
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
        return 1;
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
			perror("ERROR: connection not accepted");
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
    close (client_socket);
    
    
	return 0;
}

