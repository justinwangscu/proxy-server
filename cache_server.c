
// some peculiarities:
// if important information in the headers are cut off at the end of net_buff buffer, then it may not be parsed correctly.
// for example if the buffer ended with "Content-Length: 1" it would be read as 1. But the next buffer may start with "000000000000\n" and it would be ignored.
// there are cases when servers do not respond to the If-Modified-Since Header. To ensure expected behavior we will check the response header for "Last-Modified" and compare them ourselves

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

#include <sys/stat.h>
#include <time.h>
#include <dirent.h>


#define SOCK_READ_BUFF  8192		// Size of the buffer used to store the bytes read over socket (make this big or else bugs)
#define FILE_READ_BUFF  8192		// Size of the buffer used to store the bytes read from file

#define REQ_BUFF		8192		// Size of the buffer used to store requests from browser
#define RES_BUFF      	8192        // Size of the buffer used to store response to/from server
#define HOST_BUFF		64			// Buffer size for host name 
#define CONLEN_BUFF		128			// Buffer size for host name
#define URL_BUFF        200
#define GENERAL_BUFF    100
#define FILEPATH_MAX    500

#define SERVER_BACKLOG  100		    // Number of connections allowed 
#define THREAD_POOL_LEN 1		    // Number of threads allowed 


#define SERVER_PORT		3003		// Port number of server
#define CLIENT		    80		    // Port number of client

#define MAXLINE 		10

#define CACHE_DIR       "cachedfiles/"


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
    
    printf("closed client_socket\n");
    write(STDERR_FILENO, "Caught SIGINT\n", 15);
}

void sanitize_url(char* input) {
    int i;
    int len = strnlen(input, URL_BUFF);
    for(i = 0; i < len; i++) {
        char b = input[i];
        // windows and linux/unix forbidden ASCII characters
        if(b=='\\' || b=='/' || b=='<' || b=='>' || b==':' || b=='\"' || b=='|' || b=='?' || b=='*' || b<=31) {
            b = '-';
        }
        input[i] = b;
    }
    return;
}


int get_url(char* dest, const char* const request) {
    char *startP, *endP;
    size_t lineLen;
    char temp_buff[URL_BUFF] = {0};
    
    // set pointer to start at "GET "
    startP = strstr(request, "GET ");	

    if(startP == NULL) {
        perror("no \"GET\" detected");
        return 0;
    }
    
    // set start after "GET "
    startP += strlen("GET ");	
    // set end before next space
    endP = strstr(startP, " ");		

    // copy line into hostname
    lineLen = endP - startP;				// get length of line
    if ((lineLen + 1 > URL_BUFF / sizeof(char)) || lineLen < 0) {
        //printf("Linelen: %lu is too long\n\n", lineLen);
        return 0;
    }
    strncpy(temp_buff, startP, lineLen);		// copy line from start of line to end into hostname
    // temp_buff[lineLen] = '\0';					// add null terminator 
    // printf("One line: %s\n\n", hostname);

    // set start pointer to beginning of temp
    startP = temp_buff;
    // set end pointer to end of temp 
    endP = &temp_buff[lineLen - 1];

    // if url starts with http: skip after it
    int containsHttp = strncmp(temp_buff, "http:", strlen("http:"));
    if(containsHttp == 0) {     // if starts with http
        startP = strstr(temp_buff, "//");
        startP += strlen("//");
        // printf("After \"//\": %s\n\n", startP);
    }

    // recopy final string into hostname
    lineLen = endP - startP + 1;					// get length of line
    if (lineLen + 1 > (HOST_BUFF / sizeof(char)) || lineLen < 0) {
        //printf("Linelen: %lu is too long\n\n", lineLen);
        return 0;
    }

    strncpy(dest, startP, lineLen);			// copy from start to end into hostname
    // dest[lineLen] = '\0';				    // add null terminator

    fprintf(stderr, "URL in get_url(): %s\n", dest);

    return 1;
}

int send_dummy_response_to_client(const int client_connection) {
    size_t      soc_written = 0;                // num bytes written to a socket
    char res_buff [RES_BUFF];

    sprintf(res_buff, "HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/plain\r\nContent-Length: %d\r\n\r\n", 0);
    soc_written = write(client_connection, res_buff, strlen(res_buff));
    if (soc_written < 0) {
        perror("Error writing to client socket\n");
    }
    return soc_written;
}


// puts host string into dest, reads from request
// INPUT: dest should be size HOST_BUFF, request is buffer holding http request
// 0 = couldn't find host string, 1 = found host string
int get_host_string(char* dest, const char* const request) {
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
int connect_using_hostname(const char* const host, int *pserver_socket) {
    //printf("connect_using_hostname()\n\thost string: %s\n\n", host);

	struct addrinfo hints, *result, *rp;

	int errcode;

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

int connect_to_host(int* server_socket, const char* const req) {
    char    hostname[HOST_BUFF];            // buffer to hold host name

    /* --------------- Get hostname string in request -------------------- */

    int foundHostName = get_host_string(hostname, req);
    if (!foundHostName) {       // if we didn't get a hostname string -> send dummy response
        fprintf(stderr, "Error: Could not get host name from request\n\n");
        return 0;
    }

    // if we sucessfully found a hostname string

    /* --------------- Connect to Server -------------------- */
    int connectedToHost = connect_using_hostname(hostname, server_socket);
    if(!connectedToHost) {      // if we couldn't connect to server -> send dummy response
        fprintf(stderr, "connection error: %d\n", connectedToHost);
        return 0;
    }

    return 1;

}

// sets foundConLen to 0 if Content-Length header not found, to 1 if found
// returns contentLength if found, returns 0 if not found
size_t get_content_length(const char* const response, int *foundConLen) {
    char        conLenString[CONLEN_BUFF];      // buffer to hold Content-Length String

	// printf("response: \" %s\n\n", response);

	char* startP = strstr(response, "Content-Length: ");	// set pointer to start at "Host: " line
    char* endP;

    size_t conLen;

	if(startP == NULL) {
		// fprintf(stderr, "no \"Content-Length: \" detected\n");
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


// returns 0 if no end found, copies response header into response buffer, sets foundHeaderEnd
size_t copy_bytes_to_header_end(const char* const response, char* res_header_buff, int* foundHeaderEnd) {
    const char* startP, *endP;
    startP = response; 
    
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

    // header length = start of "\r\n\r\n" - start of buffer + len of "\r\n\r\n" (should be 4)
    length = (endP - startP) + strlen("\r\n\r\n");
    // printf("length to header end: %lu\n", length);
    if(length > RES_BUFF) {
        copy_length = RES_BUFF;
    }
    else {
        copy_length = length;
    }

    // copy header string
    strncpy(res_header_buff, startP, copy_length);
    res_header_buff[copy_length] = '\0';        // add null terminator
    // fprintf(stderr, "Response Header: \n\n%s\n\n", res_header_buff);

    // return header length
    return length;
}

// returns 0 if not found, 1 if found
int chunked_encoding_check(const char* const response) {
    char* startP = strstr(response, "Transfer-Encoding: chunked");
    if (startP == NULL) return 0;

    // printf("This response has \"Transfer-Encoding: chunked\" and has no ContentLength\n");
    return 1;
}

// used to detect end of chunked encoding stream 
// only call after detecting response header end
// returns 0 if not found, 1 if found
int contains_end_of_stream(const char* const response) {
    const char *endP;
    endP = response;

    // check for CRLF
    endP = strstr(response, "0\r\n\r\n");
    if(endP == NULL) {
        // printf("No CRLF yet\n");
        return 0;
    }
    
    // printf("EOS detected in response: \n%s\n At endP: %s\n", startP, endP);
    return 1;
}

// returns 1 if 200 is found on first line, 0 otherwise
int is_200_response(const char* const net_buff) {
    char *firstLine = strstr(net_buff, "\n");
    char *firstTwoHundred = strstr(net_buff, "200");

    // if "200" not found or "200" is after the first line -> non-200 response
    if (firstTwoHundred == NULL || firstTwoHundred >= firstLine) {
        //printf("Non-200 Response Detected\n");
        return 0;
    }
    // if "200" found on first line
    else {
        return 1;
    }
}

// reads and sends the cached file in filepath in CACHE_DIR directory
int read_and_send_cached_file(const char* const filepath, const int client_connection) {
    FILE *fptr;
    size_t file_readed = 0;
    size_t soc_written;
    
    char file_buff[FILE_READ_BUFF];

    fptr = fopen(filepath, "rb");
    if(fptr == NULL) {
        perror("file open error in read_and_send_cached_file()");
        return -1;
    }

    
    while((file_readed = fread(file_buff, 1, sizeof(file_buff), fptr)) > 0) {
        soc_written = write(client_connection, file_buff, file_readed);
        if (soc_written < 0) {
            perror("Error writing to client socket\n");
            return -1;
        }
    }

    return 1;
}

struct response_info {
    // int         foundChunkedEncoding;       // true when chuncked encoding found in response header
    int         foundConLen;            // true when content-length found in response header
    int         foundHeaderEnd;         // true when end of the response header found in net_buff
    int         found200;               // true when we found 200 response code
    int         responseDone;           // true when EOS is found after header OR content_length indicates end of response
    int         isCacheable;
    int         responseCodeFound;
    // these don't pertain to the response itself
    int         error;
    int         readError;
    int         writeError;
    int         fileError;
    int         readZeroBytes;


    size_t      bytesToHeaderEnd;       // bytes from the start of net_buff to the end of the header

    size_t      contentLength;          // content length in defined in response header 

    size_t      content_readed;         // number of bytes read after response header
    size_t      total_response_read;    // number of bytes read from response

    size_t      headerSize;             // header size
};

struct socket_descriptors {
    int         client_connection;
    int         server_socket;
    size_t      soc_readed;                 // var for storing bytes read from socket on any given read
    size_t      soc_written;                // var for storing bytes written to socket on any given write
};

// reads from server socket and sends response until header end 
// keeps track of response code header size, content-length
// returns 0 if nothing unusual, -1 if error, 1 if weird case
// TODO: check for Cache-Control: no-cache
int handle_response_header(struct socket_descriptors* sd, char* net_buff, char* res_header_buff, struct response_info* ri, const int onlySendIf200) {
    ri->responseCodeFound = 0;

    /* ------------------- Send Response from Server to Client -----------------*/
    //  Keep reading from server and sending to client until:
    //      we find the end of header
    do {
        memset(net_buff, 0, SOCK_READ_BUFF);  // clear net_buff

        // fprintf(stderr, "reading... in handle_response_header()\n");
        
        // read from socket
        sd->soc_readed = read(sd->server_socket, net_buff, SOCK_READ_BUFF); 
        if (sd->soc_readed < 0) {   // check read error
            perror("ERROR reading from socket\n"); // if read returns < -1 then error
            ri->error = 1;
            ri->readError = 1;
            ri->responseDone = 1;
            return -1;
        }
        if(sd->soc_readed == 0) { // if readed is 0 -> break
            perror("soc_readed == 0 in handle_response_header()");
            ri->responseDone = 1;
            ri->readZeroBytes = 1;
            return 1;
        }  


        // printf("%lu read from server\n", soc_readed);
        ri->total_response_read += sd->soc_readed;

        // fprintf(stderr, "response header read\n");
        
        // Try to find response code (assuming it's on the first line)
        if(!ri->responseCodeFound) {
            // response code should be after first space
            // char* afterfirstSpace = strstr(net_buff, " ");
            // afterfirstSpace + 1;

            // // copy first 3 characters after first space from net_buff to resCode;
            // strncpy(resCode, afterfirstSpace, 3);

            ri->found200 = is_200_response(net_buff);
            ri->responseCodeFound = 1;
        }

        // if we don't want to send non-200 requests -> break (assume code is found on first read)
        if(onlySendIf200 && !ri->found200) {
            ri->responseDone = 1;
            return 0;
        }


        // Try to find "Content-Length"
        if(!ri->foundConLen) {            
            ri->contentLength = get_content_length(net_buff, &ri->foundConLen);
            if(ri->foundConLen) {
                // printf("Content Len is: %lu\n", ri->contentLength);   
                //ri->foundConLen = 1;
            }
        }

        // Try to find header end
        // also copy header into res_header_buff
        ri->bytesToHeaderEnd = copy_bytes_to_header_end(net_buff, res_header_buff, &ri->foundHeaderEnd);
        // fprintf(stderr, "bytesToHeaderEnd: %lu\n", ri->bytesToHeaderEnd);
        
        // Adjust header size
        if(!ri->foundHeaderEnd) {
            ri->headerSize += sd->soc_readed;   
        }
        else {
            ri->headerSize += sd->soc_readed - ri->bytesToHeaderEnd;
        }

        // weird edge case where we haven't found the response code in the first buffer.
        if(!ri->responseCodeFound && onlySendIf200) {
            perror("response code not found");
            return 1;
        }
        
        // --------------------- Send response to client -----------------
        // send response from server
        sd->soc_written = write(sd->client_connection, net_buff, sd->soc_readed);
        if (sd->soc_written < 0) {
            perror("Error writing to client socket\n");
            ri->writeError = 1;
            ri->error = 1;
            ri->responseDone = 1;
            return -1;
        }
        // printf("%lu written to client\n", sd->soc_written);

    } while(!ri->foundHeaderEnd);

    // fprintf(stderr, "response header handled\n");

    return 0;
}

// call only if net_buff contains the end of the header
// reads and sends the rest of the response and caches it if applicable
// make sure url/filepath is sanitized already
// set doCache to 1 if you want cacheable responses to be cached
int forward_response_with_cache_option(struct socket_descriptors* sd, char* net_buff, const char* const filepath, struct response_info* ri, int doCache) { 
    // stores response body in cache if cacheable
     
    // size_t      numContentChars = 0; 

    FILE *fptr;

    // stop if header end was not found/there was an error or the response code was not 200
    if(ri->error || !ri->foundHeaderEnd || !ri->found200) {
        // perror("handle_response_header() error");
        return -1;
    }
    
    // if just we found header end  
    // If we didn't find Con Len
    //  -> look out for end of stream token
    if (!ri->foundConLen) {
        if(contains_end_of_stream(net_buff)) {
            ri->responseDone = 1;
        }
    }

    // content_readed is size of the message read - part of the message that is the header
    ri->content_readed = sd->soc_readed - ri->bytesToHeaderEnd;
    ri->headerSize += ri->bytesToHeaderEnd;

    // if content length hit -> break
    if(ri->foundConLen) {
        // if we read all the content we need to -> stop reading
        if(ri->content_readed >= ri->contentLength) {
            ri->responseDone = 1;
        }
    }
    
    // right now whether a file is cachable is whether content length is specified and whether it's less than 100 KiB
    // if contentLength is greater than 102,400 bytes (100 KiB) -> don't cache it
    // if cacheable -> cache the response body part of the buffer
    // open file in write bytes mode
    if(doCache && ri->found200 && !(ri->foundConLen && ri->contentLength > 102400)) {
        ri->isCacheable = 1;

        fptr = fopen(filepath, "wb");
        if(fptr == NULL) {
            fprintf(stderr, "Bad Filepath?: %s\n", filepath);
            perror("Error opening file, check perms, maybe restart computer");

            ri->error = 1;
            ri->fileError = 1;
        }
        else {
            fprintf(stderr, "CREATED/OPENED %s IN wb MODE\n", filepath);

            // write bytes read from server to fptr
            if(fwrite(net_buff, 1, sd->soc_readed, fptr) != sd->soc_readed) {
                perror("fwrite() error1 in forward_response_with_cache_option");
                ri->error = 1;
                ri->fileError = 1;
                fclose(fptr);

                fprintf(stderr, "DELETING %s\n", filepath);
                // delete file
                if(remove(filepath) != 0) {
                    perror("remove() error");
                    ri->error = 1;
                    ri->fileError = 1;
                }
            }
        }

        // this was unneccesary it's better to write the resposne header to the file
        // // write content after header to cache
        // char* startOfContent = net_buff + ri->bytesToHeaderEnd;
        // size_t endOfHeaderToEndOfBuffer = sizeof(net_buff) - ri->bytesToHeaderEnd;
        // // 
        // numContentChars = strstr(startOfContent, "0\r\n\r\n") == NULL ? endOfHeaderToEndOfBuffer : (strstr(startOfContent, "0\r\n\r\n") - startOfContent);
        // fptr = fopen(filepath, "wb");
        // if(fptr == NULL) {
        //     perror("Error opening file");
        //     return 0;
        // }

        // fwrite(startOfContent, sizeof(char), numContentChars, fptr);
    }

    // return if responseDone
    if(ri->responseDone) {
        return 0;
    }

    /* ------------------- Send Response from Server to Client -----------------*/
    //  Keep reading from server and sending to client until:
    //      if no Content Length
    //              -> stop when end of stream token is found
    //      if "Content-Length: " was found in HTTP response header
    //          if data delivered after header >= content-length
    //              -> stop
    do {
        memset(net_buff, 0, SOCK_READ_BUFF);  // clear net_buff

        // read from socket
        sd->soc_readed = read(sd->server_socket, net_buff, SOCK_READ_BUFF); 
        if (sd->soc_readed < 0) {   // check read error
            perror("ERROR reading from socket\n"); // if read returns < -1 then error
            ri->readError = 1;
            ri->error = 1;
            return -1;
        }
        if(sd->soc_readed == 0) { // if readed is 0 -> break
            perror("soc_readed == 0 in forward_response_with_cache_option()");
            ri->readZeroBytes = 1;
            break;
        }  

        // printf("%lu read from server\n", sd->soc_readed);
        ri->total_response_read += sd->soc_readed;

        // --------------------- Send response to client -----------------
        // send response from server
        sd->soc_written = write(sd->client_connection, net_buff, sd->soc_readed);
        if (sd->soc_written < 0) {
            perror("Error writing to client socket\n");
            ri->error = 1;
            ri->writeError = 1;
            return -1;
        }
        // printf("%lu written to client\n", sd->soc_written);

        // keep track of content read after header
        ri->content_readed += sd->soc_readed;

        // write to cache
        if(!ri->fileError && ri->isCacheable && ri->content_readed <= 102400) {
            if(fwrite(net_buff, sizeof(char), sd->soc_readed, fptr) != sd->soc_readed) {
                perror("fwrite() error2 in forward_response_with_cache_option()");
                ri->error = 1;
                ri->fileError = 1;
                fclose(fptr);

                fprintf(stderr, "DELETING %s\n", filepath);
                // delete file
                if(remove(filepath) != 0) {
                    perror("remove() error");
                    ri->error = 1;
                    ri->fileError = 1;
                    return -1;
                }
                return -1;
            }
        }
        // if bytes exceed the limit -> close and remove the file
        else if(!ri->fileError && ri->isCacheable && ri->content_readed > 102400) {
            fprintf(stderr, "FILE SIZE LIMIT REACHED\n");
            fclose(fptr);
            // delete file
            fprintf(stderr, "DELETING %s\n", filepath);
            if(remove(filepath) != 0) {
                perror("remove() error");
                ri->error = 1;
                ri->fileError = 1;
                return -1;
            }
        }

        // If we didn't find Con Len
        //  -> look out for end of stream token
        if (!ri->foundConLen) {
            if(contains_end_of_stream(net_buff)) {
                ri->responseDone = 1;
            }
        }
        // If Content-Length 
        //  -> keep track of bytes after header
        else {
            // printf("hi %lu\n", content_readed);

            // printf("contentreaded: %lu\n", content_readed);

            // if we read all the content we need to -> last loop
            if(ri->content_readed >= ri->contentLength) {
                ri->responseDone = 1;
            }

        }
         
    } while(!ri->responseDone);

    if(!ri->fileError && doCache && ri->isCacheable) {
        fclose(fptr);
    }

    return 0;
}

// caches and forwards the server response
struct response_info cache_and_forward_server_response(struct socket_descriptors* sd, char *net_buff, char* res_header_buff, const char* const filepath) {
    struct response_info ri = {0};

    // read response until header end and send regardless of response code
    handle_response_header(sd, net_buff, res_header_buff, &ri, 0);

    if(ri.error || !ri.responseCodeFound) {
        return ri;
    }

    // read, cache (if cacheable) and send the rest of server response body
    forward_response_with_cache_option(sd, net_buff, filepath, &ri, 1);
    
    return ri;
    
}


// returns 0 if the response was 200 and was forwarded to the client
// returns 1 if cached file was send
struct response_info forward_and_cache_server_response_if_resource_was_modified(struct socket_descriptors* sd, char* net_buff, char* res_header_buff, const char* const filepath) {
    struct response_info ri = {0};

    handle_response_header(sd, net_buff, res_header_buff, &ri, 1);
    // fprintf(stderr, "response header handled\n");
    
    // if 200 that means the server has modified the resource so retrieve it again, cache it and forward it
    if(!ri.error && ri.found200 && ) {
        // forward and cache response
        forward_response_with_cache_option(sd, net_buff, filepath, &ri, 1);
        return ri;
    }

    return ri;
}


struct response_info forward_server_response_without_caching(struct socket_descriptors* sd, char* net_buff, char* res_header_buff) {
    struct response_info ri = {0};
    char filepath[2] = {0};

    // forward response header regardless of code
    handle_response_header(sd, net_buff, res_header_buff, &ri, 0);
    
    // forward rest of response and do not cache
    forward_response_with_cache_option(sd, net_buff, filepath, &ri, 0);

    return ri;
}

int is_cached(const char* const filepath) {
    FILE *fptr;

    // cache hit
    fptr = fopen(filepath, "rb");
    if(fptr != NULL) {
        fclose(fptr);
        // perror("Cache Hit");
        return 1;
    }
    else {
        // perror("Cache Miss");
        return 0;
    }
}

// https://developer.mozilla.org/en-US/docs/Web/HTTP/Headers/If-Modified-Since
// sends If-Modified-Since GET request based on req_buff
int send_if_modified_since_GET(const int server_socket, const char* const req_buff, char* modified_req_buff, const char * const filepath) {
    size_t soc_written;
    struct stat fileStat;

    int lenToLastLine;
    char* endOfHeader = strstr(req_buff, "\r\n\r\n");
    char* lastNewLine = endOfHeader + strlen("\r\n");

    char ifModifiedSince[] = "If-Modified-Since: "; 
    char myTime[40] = {0};

    if(stat(filepath, &fileStat) == -1) {
        perror("fileStat error in sendModifiedGet()");
        return -1;
    }

    struct tm *timeInfo = gmtime(&fileStat.st_ctime);
    strftime(myTime, sizeof(myTime), "%a, %d %b %Y %H:%M:%S GMT", timeInfo);
    
    lenToLastLine = lastNewLine - req_buff;

    // modified_req_buff = original request up to second to last "\r\n" + "If-Modified-Since: " + <myTime> + "\r\n"
    strncat(modified_req_buff, req_buff, lenToLastLine);
    strncat(modified_req_buff, ifModifiedSince, strnlen(ifModifiedSince, 20));
    strncat(modified_req_buff, myTime, strnlen(myTime, 40));
    strncat(modified_req_buff, "\r\n\r\n", strnlen("\r\n\r\n", 4));
    

    soc_written = write(server_socket, modified_req_buff, strnlen(modified_req_buff, REQ_BUFF));

    // fprintf(stderr, "SENT MODIFIED GET (len: %lu): %s\n", strnlen(modified_req_buff, REQ_BUFF), modified_req_buff);

    char* CRLF = strstr(modified_req_buff, "\r\n\r\n");
    if(CRLF == NULL) {
        fprintf(stderr, "CRLF NOT FOUND\n");
    }
    if(*((char *)(CRLF + strlen("\r\n\r\n"))) != '\0') {
        fprintf(stderr, "MODDED REQ BUFF MAY NOT BE NULL TERMINATED\n");
    }

    if(soc_written <= 0) {
        perror("write() error in send_if_modified_since_GET()");
    }
    else {
        // perror("write() success in send_if_modified_since_GET()");
    } 

    return 1;

}


void *handle_connection(void *pclient_connection) {
    int         client_connection = *((int *)pclient_connection);   // make a copy of the int
    int         server_socket = 0;              // socket descriptor

    int isGet = 0;
    int isCached = 0;
    int isCacheHit = 0;
    int sentCachedFile = 0;
    int cacheWasOutdated = 0;
    int cacheUpdated = 0;;
    int cacheEntryCreated = 0;

    // size_t      total_written = 0;              // total bytes written
    // size_t      total_read = 0;

    size_t      soc_readed = 0;                 // num bytes read from a socket
    size_t      soc_written = 0;                // num bytes written to a socket


	char        net_buff[SOCK_READ_BUFF] = {0};       // buffer to hold characters read from socket
	char        req_buff[REQ_BUFF] = {0};				// buffer to hold the request
    char        modified_req_buff[REQ_BUFF] = {0};	// buffer to hold the modified GET request
	char        res_header_buff[RES_BUFF] = {0};      // buffer to hold the response header
	// char        hostname[HOST_BUFF] = {0};            // buffer to hold host name
    char        resCode[4] = {0};               // buffer to hold response code + null terminator
    char        url[URL_BUFF] = {0};            // holds url
    char        filepath[FILEPATH_MAX] = CACHE_DIR;            // holds file path

    struct response_info ri = {0};

    // Free argument memory
    //printf("free pclient_connection\n");
    free(pclient_connection);
    pclient_connection = NULL;

    // set buffers with null terminators
    // memset(net_buff, '\0', sizeof(net_buff));
    // memset(req_buff, '\0', sizeof(req_buff));
    // memset(modified_req_buff, '\0', sizeof(modified_req_buff));
    // memset(res_header_buff, '\0', sizeof(res_header_buff));
    // memset(hostname, '\0', sizeof(hostname));
    // memset(resCode, '\0', sizeof(resCode));
    // memset(url, '\0', sizeof(url));

    struct socket_descriptors sd = {0};
    sd.client_connection = client_connection;
    
    //printf("Handling Client Connection: %d\n\n", client_connection);

    /* --------------- Read incoming request ------------------------- */
    
    // read request from client socket
    soc_readed = read(client_connection, net_buff, SOCK_READ_BUFF);
    if(soc_readed < 0) {
        perror("ERROR reading from socket");
        goto closeConnections;
    }

    // copy request into req_buff
    size_t reqSize = strlen(net_buff);
    strncpy(req_buff, net_buff, sizeof(req_buff));

    // // print request
    // printf("Request Size %d: \nRequest String:%.*s\n", reqSize, reqSize, req_buff);

    // if request is empty -> send dummy response to client
    if(reqSize == 0) {
        soc_written = send_dummy_response_to_client(client_connection);
        goto closeConnections;
    }


    /* --------------- Connect to Host -------------------- */
    int foundHost = connect_to_host(&server_socket, net_buff);
    if(!foundHost) {
        soc_written = send_dummy_response_to_client(client_connection);
        goto closeConnections;
    }
    sd.server_socket = server_socket;

    /* --------------- Check for Cache Hits -------------------- */
    
    // if GET request -> check for cache hits
    if(strncmp("GET", req_buff, strlen("GET")) == 0) {
        isGet = 1;
        get_url(url, req_buff);
        sanitize_url(url);
        // filepath = ./directory/url
        strncat(filepath, url, strnlen(url, URL_BUFF - strlen(filepath) - 1)); // append url to directory path

        fprintf(stderr, "URL: %s\n", url);
        isCached = is_cached(filepath);
    }
    
    // if non-GET request
    if(!isGet) {
        /* ------------------- send original request to server ---------------- */        
        soc_written = write(server_socket, net_buff, soc_readed);
        ri = forward_server_response_without_caching(&sd, net_buff, res_header_buff);
    }
    // if item is in the cache -> send modified GET, if Response is 200 -> cache the response, else -> send cached file to client 
    else if(isCached) {
        isCacheHit = 1;
        /* ------------------- Send modified GET to server ---------------- */        
        send_if_modified_since_GET(server_socket, req_buff, modified_req_buff, filepath);

        // fprintf(stderr, "sent modified Get\n");

        // Read response
        ri = forward_and_cache_server_response_if_resource_was_modified(&sd, net_buff, res_header_buff, filepath);
        // fprintf(stderr, "read response header to modded GET\n");

        // if response is not 200:
        if(!ri.error && !ri.found200) {
            /* ------------------- Send Cached File to Client ---------------- */        
            // fprintf(stderr, "response to modded GET not 200, sending cached file \n");
            read_and_send_cached_file(filepath, client_connection);
            sentCachedFile = 1;
        }
        // if response was 200 then the forward_and_cache_server_response_if_resource_was_modified() call forwarded and cached the response
        else {
            // fprintf(stderr, "response to modded GET was 200, response was forwarded \n");
            cacheWasOutdated = 1;
            cacheUpdated = 1;
        }
    }

    // if item is not in the cache -> send original request and cache the response
    else if(!isCached) {
        /* ------------------- Send original request to server ---------------- */        
        soc_written = write(server_socket, req_buff, soc_readed);
        /* ------------------- Cache and Forward Response from Server ---------------- */   
        ri = cache_and_forward_server_response(&sd, net_buff, res_header_buff, filepath);
        cacheUpdated = 1;
        cacheEntryCreated = 1;
    }
    
    
    //printf("Wrote %lu chars to server connection\n\n", soc_written);


    // printf("Response Total Length: %lu\n", ri->total_response_read);
    // printf("Content Read: %lu\n\n", ri->content_readed);

    
    printf("Original Request");
    if(isGet) {
        printf(" %s ", url);
    }
    if(ri.foundConLen) {
        printf(" (Content-Length: %lu):\n", ri.contentLength);
    }
    else {
        printf(":\n");
    }
    printf("%s\n", req_buff);

    if(isCacheHit) {
        printf("Modified Request");
        printf(" %s", url);
        printf(":\n%s\n", modified_req_buff); 

        printf("Modified Response");     
        printf(" %s", url);  
        printf(":\n%s\n", res_header_buff);
    }
    
    if(!isCacheHit) {
        printf("Response %s (Content Read: %lu)", url, ri.content_readed);
        printf(":\n%s\n\n", res_header_buff);
    }

    if(sentCachedFile) {
        printf("Sent Cached Entry in %s\n\n", filepath);
    }
    
    if(cacheEntryCreated) {
        printf("Cache Entry Created in %s\n\n", filepath);
    }
    else if(cacheUpdated) {
        printf("Cache Entry Updated in %s\n\n", filepath);
    }
    
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

