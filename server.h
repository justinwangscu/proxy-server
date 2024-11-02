#ifndef _PROXY_SERVER
#define _PROXY_SERVER

// some peculiarities:
// if important information in the headers are cut off at the end of net_buff buffer, then it may not be parsed correctly.
// for example if the buffer ended with "Content-Length: 1" it would be read as 1. But the next buffer may start with "000000000000\n" and it would be ignored.
// there are cases when servers do not respond to the If-Modified-Since Header. To ensure expected behavior we will check the response header for "Last-Modified" and compare them ourselves
// make sure strstr is only called on buffers with a null termination. or you could switch to strnstr(a, b, sizeof(a))

#define _XOPEN_SOURCE 700   // for strptime

#include <stdlib.h>

struct response_info {
    int         foundChunkedEncoding;       // true when chunked encoding found in response header
    int         foundConLen;            // true when content-length found in response header
    int         foundHeaderEnd;         // true when end of the response header found in net_buff
    int         found200;               // true when we found 200 response code
    int         responseDone;           // true when EOS is found after header OR content_length indicates end of response
    int         isCacheable;
    int         responseCodeFound;
    int         resourceWasNotModified; // 0 = not sure, could be modified; 1 = for sure it was not modified
    // these don't pertain to the response itself
    int         error;
    int         readError;
    int         writeError;
    int         fileError;
    int         readZeroBytes;

    int         cacheUpdated;
    int         tooLarge;


    size_t      bytesToHeaderEnd;       // bytes from the start of net_buff to the end of the header

    size_t      contentLength;          // content length in defined in response header 

    size_t      content_readed;         // number of bytes read after response header
    size_t      total_response_read;    // number of bytes read from response

    size_t      headerSize;             // header size
};

struct socket_descriptors {
    int         client_connection;
    int         server_socket;
    int      soc_readed;                 // var for storing bytes read from socket on any given read
    int      soc_written;                // var for storing bytes written to socket on any given write
};

void sanitize_url(char* input);

int get_url(char* dest, const char* const request);

int send_dummy_response_to_client(const int client_connection);


// puts host string into dest, reads from request
// INPUT: dest should be size HOST_BUFF, request is buffer holding http request
// 0 = couldn't find host string, 1 = found host string
int get_host_string(char* dest, const char* const request);

// Input: pointer dest is passed by reference
// Result: creates socket and connection to host
// returns 1 if successful connection, 0 if failed
int connect_using_hostname(const char* const host, int *pserver_socket);


// sets foundConLen to 0 if Content-Length header not found, to 1 if found
// returns contentLength if found, returns 0 if not found
size_t get_content_length(const char* const response, int *foundConLen);

// returns 0 if no end found, copies response header into response buffer, sets foundHeaderEnd
size_t copy_bytes_to_header_end(const char* const response, char* res_header_buff, int* foundHeaderEnd);

// returns 0 if not found, 1 if found
int is_chunked_encoding(const char* const response);

// used to detect end of chunked encoding stream 
// only call after detecting response header end
// returns 0 if not found, 1 if found
int contains_end_of_stream(const char* const response);


// copies Last-Modified Value String into dest
// make sure dest is big enough 
int get_last_modified_string(char* dest, char *res_header_buff);

// returns 1 if 200 is found on first line, 0 otherwise
int is_200_response(const char* const net_buff);

// reads and sends the cached file in filepath in CACHE_DIR directory
int read_and_send_cached_file(const char* const filepath, char* res_header_buff, const int client_connection);

// reads from server socket and sends response until header end 
// keeps track of response code header size, content-length
// returns 0 if nothing unusual, -1 if error, 1 if weird case
// TODO: check for Cache-Control: no-cache
int handle_response_header(struct socket_descriptors* sd, char* net_buff, char* res_header_buff, struct response_info* ri, const int onlySendIf200);

// call only if net_buff contains the end of the header
// reads and sends the rest of the response and caches it if applicable
// make sure url/filepath is sanitized already
// set doCache to 1 if you want cacheable responses to be cached
int forward_response_with_cache_option(struct socket_descriptors* sd, char* net_buff, const char* const filepath, struct response_info* ri, int doCache);

// call after reading header
// keep reading until response is done
int only_read_response_after_header(struct socket_descriptors* sd, char* net_buff, struct response_info* ri);


// caches and forwards the server response
struct response_info cache_and_forward_server_response(struct socket_descriptors* sd, char *net_buff, char* res_header_buff, const char* const filepath);


// returns 0 if the response was 200 and was forwarded to the client
// returns 1 if cached file was send
struct response_info forward_and_cache_server_response_if_resource_was_modified(struct socket_descriptors* sd, char* net_buff, char* res_header_buff, const char* const filepath);


struct response_info forward_server_response_without_caching(struct socket_descriptors* sd, char* net_buff, char* res_header_buff);

int is_cached(const char* const filepath);

// https://developer.mozilla.org/en-US/docs/Web/HTTP/Headers/If-Modified-Since
// sends If-Modified-Since GET request based on req_buff
int send_if_modified_since_GET(const int server_socket, const char* const req_buff, char* modified_req_buff, const char * const filepath);


void *handle_connection(void *pclient_connection);


#endif
