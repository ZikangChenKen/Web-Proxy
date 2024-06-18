/*
 * COMP 321 Project 6: Web Proxy
 *
 * This program implements a multithreaded HTTP proxy.
 *
 * Zikang Chen zc45, Yuqi Chen yc138.
 */ 

#include <assert.h>

#include "csapp.h"

#define NTHREADS 10         /* Number of threads. */
#define NITEMS 30			/* Number of items in shared buffer. */

// Hold the information in each thread.
struct sbuf {
	int num;
	int connfd;
	char *host_name;
	char *port;
	struct sockaddr_in clientaddr;
};

FILE *log_file;
pthread_mutex_t mutex;	/* pthread mutex. */ 
pthread_mutex_t log_mutex; 
pthread_cond_t cond_empty;/* pthread cond variable to wait on empty buffer.*/
pthread_cond_t cond_full;/* pthread cond variable to wait on full buffer.*/
unsigned int prod_idx = 0; /* Index for producer into shared buffer. */ 
unsigned int con_idx = 0; /* Index for consumer into shard buffer. */
int shared_cnt = 0;
int total_cnt = 0;
struct sbuf *sbuf_array[NITEMS];

static void	client_error(int fd, const char *cause, int err_num, 
		    const char *short_msg, const char *long_msg);
static char    *create_log_entry(const struct sockaddr_in *sockaddr,
		    const char *uri, int size);
static int	parse_uri(const char *uri, char **hostnamep, char **portp,
		    char **pathnamep);
static void doit (int fd, struct sockaddr_in *clientaddrp,
			unsigned int idx);
void		*work(void *vargp);

/* 
 * Requires:
 *   <"argc" should be 2 and argv should be valid.> 
 *
 * Effects:
 *   <Initialize the setting of threads and thread locks. Also, connect
 *    the clients to the proxy server.> 
 */
int
main(int argc, char **argv)
{
	struct sockaddr_in clientaddr;
	socklen_t clientlen = sizeof(struct sockaddr_in);
	int connfd, listenfd, i;
	char host_name[MAXLINE];
	char port[MAXLINE];
	pthread_t tids[NTHREADS];

	//Set up locks.
	Pthread_mutex_init(&log_mutex, NULL);
	Pthread_mutex_init(&mutex, NULL);
	Pthread_cond_init(&cond_empty, NULL);
	Pthread_cond_init(&cond_full, NULL);

	Signal(SIGPIPE, SIG_IGN);

	// Open the file.
	log_file = fopen("proxy.log", "a");

	if (argc != 2) {
		fprintf(stderr, "usage: %s <port>\n", argv[0]);
		exit(1);
	}

	listenfd = Open_listenfd(argv[1]);

	if (listenfd < 0) {
		client_error(listenfd, host_name, 404, "Not found", "Server not found");
	}

	// Create threads and do the work.
	for (i = 0; i < NTHREADS; i++) {
		Pthread_create(&tids[i], NULL, work, NULL);
	}

	while (1) {

		struct sbuf *buffer = Malloc(sizeof(struct sbuf));
		connfd = Accept(listenfd, (struct sockaddr *)&clientaddr, &clientlen);
		getnameinfo((struct sockaddr *) &clientaddr, clientlen, host_name,
			MAXLINE, port, MAXLINE, 0);
		
		//Set up buffer.
		buffer->connfd = connfd;
		buffer->clientaddr = clientaddr;
		buffer->host_name = host_name;
		buffer->port = port;
		buffer->num = total_cnt;

		Pthread_mutex_lock(&mutex);

		// Check if the buffer is full.
		while (shared_cnt == NITEMS){
			Pthread_cond_wait(&cond_empty, &mutex);
		}

		total_cnt ++;
		sbuf_array[prod_idx] = buffer;
		Pthread_cond_broadcast(&cond_full);

		if (prod_idx == NITEMS - 1)
			prod_idx = 0;
		else
			prod_idx ++;

		shared_cnt ++;
		Pthread_mutex_unlock(&mutex);	
	}
	// Destroy the lock.
	Pthread_mutex_destroy(&mutex);
	Pthread_mutex_destroy(&log_mutex);
	Pthread_cond_destroy(&cond_empty);
	Pthread_cond_destroy(&cond_full);
	Fclose(log_file);

	/* Return success. */
	return (0);
}

/* 
 * Requires:
 *   <"vargp" is a valid struct sbuf pointer.> 
 *
 * Effects:
 *   <Control the threads in thread buffer.> 
 */
void
*work(void *vargp) 
{
	(void)vargp;
	Pthread_detach(pthread_self());
	
	struct sbuf *buffer;
	while (1) {
		pthread_mutex_lock(&mutex);
		
		//Control the number of threads in buffer.
		while (shared_cnt == 0) {
			Pthread_cond_wait(&cond_full, &mutex);
		}

		buffer = sbuf_array[con_idx];
		if (con_idx == NITEMS - 1)
			con_idx = 0;
		else
			con_idx ++;
		
		shared_cnt -= 1;
		Pthread_mutex_unlock(&mutex);
		Pthread_cond_broadcast(&cond_empty);

		doit(buffer -> connfd, &buffer -> clientaddr, buffer->num);
		Close(buffer -> connfd);
	}
	return (NULL);
}


/* 
 * Requires:
 *   <Nothing.> 
 *
 * Effects:
 *   <Handle one HTTP request/response transaction.> 
 */
void
doit(int fd, struct sockaddr_in *clientaddrp, unsigned int idx) 
{

	int connfd, serverfd, n, bytes_sent, hdr_len, first_idx, num_buf;
	//struct request_info *req_info;
	rio_t rio_client, rio_server;
	char method[MAXLINE], uri[MAXLINE], http[MAXLINE], *buffer,
	first_hdr[MAXLINE];
	char *port, *host_name, *path_name, *log;
	char haddrp[INET_ADDRSTRLEN];
	bytes_sent = 0;
	//req_info = (struct request_info *) vargp;
	connfd = fd;


	// Initialize rio with conn pointer.
	rio_readinitb(&rio_client, fd);

	// Allocate memory for buf.
	buffer = Malloc(MAXLINE);
	first_idx = 0;
	num_buf = 1;

	// Checks first request header.
	while ((n = rio_readlineb(&rio_client, &buffer[first_idx], MAXLINE)) > 0) {
		if (n == MAXLINE - 1) {
			num_buf = 2;
			Realloc(buffer, num_buf * MAXLINE);
			first_idx += n;
			continue;
		} else {
			break;
		}
	}

	// check whether request is normal and supported;
	// Read the returned error message to modify

	if (n < 0) {
		client_error(connfd, buffer, 400, "Bad request", 
					"Bad request, check request header");
		return;
	}

	if (sscanf(buffer, "%s %s %s", method, uri, http) != 3) {
		client_error(connfd, buffer, 400, "Bad request", 
					"Bad request, check 3 req() parts");
		return;
	}

	if (strncmp(method, "GET", 3) != 0) {
		client_error(connfd, buffer, 501, "Not implemented", 
					"Bad requested method, check method");
		return;
	}

	if (!strncmp(http, "HTTP/1.0", 8) != 0 && !strncmp(http, "HTTP/1.1", 8)) {
		client_error(connfd, buffer, 505, "HTTP version not supported", 
					"Wrong HTTP version request");
		return;
	}

	if (parse_uri(uri, &host_name, &port, &path_name) == -1) {
		client_error(connfd, buffer, 400, "Bad Request", 
					"Bad request, check URL");
		return;
	}

	// Rewrite start line to only send path.
	hdr_len = strlen(method) + strlen(path_name) + strlen(http) + 5;
	snprintf(first_hdr, hdr_len, "%s %s %s\r\n", method, path_name, http);

	Inet_ntop(AF_INET, &(clientaddrp -> sin_addr), haddrp, INET_ADDRSTRLEN);
	printf("Request %u: Received request from client (%s):\n", idx, haddrp);
	printf("%s\n\n", buffer);


	// Connect to server now
	if ((serverfd = open_clientfd(host_name, port)) == -1) {
		client_error(connfd, buffer, 404, "Not found", 
					"Could not find the server");
		return;
	}
	rio_readinitb(&rio_server, serverfd);

	if ((rio_writen(serverfd, first_hdr, strlen(first_hdr))) < 0) {
		client_error(serverfd, buffer, 400, "Bad request", 
					"Bad request, failed header");
		return;
	}

	// Display request that has been sent to server.
	printf("\n");
	printf("*** End of Request ***\n");
	printf("Request %d: Forwarding request to server:\n", 
		   idx);
	printf("%.*s", (int) strlen(first_hdr), first_hdr);

	// Read rest headers and remove 3 unnecessary types.
	while ((n = rio_readlineb(&rio_client, buffer, MAXLINE)) > 0) {
		if (strncmp(buffer, "\r\n", 2) == 0) {
			break;
		} else if (strncmp(buffer, "Connection", 10) == 0) {
			printf("Request %d: Stripping \"Connection\" header\n", 
				   idx);
			continue;
		} else if (strncmp(buffer, "Keep-Alive", 10) == 0) {
			printf("Request %d: Stripping \"Keep-Alive\" header\n", 
				   idx);
			continue;
		} else if (strncmp(buffer, "Proxy-Connection", 16) == 0) {
			printf("Request %d: Stripping \"Proxy-Connection\" header\n", 
				   idx);
			continue;
		} else {
			printf("%s", buffer);
			if ((rio_writen(serverfd, buffer, n)) < 0) {
				client_error(serverfd, buffer, 400, "Bad request",
							"Bad request, buffer can't store headers");
			}
		}
	}

	// Close the writes
	rio_writen(serverfd, "Connection: close\r\n", 19);
	rio_writen(serverfd, "\r\n", 2);

	// Record server response
	while ((n = rio_readnb(&rio_server, buffer, MAXLINE)) > 0) {
		bytes_sent += n;
		if ((rio_writen(connfd, buffer, n)) < 0) {
			client_error(serverfd, buffer, 400, "Bad request",
			 			"Bad request, server response unavailable");
		}
	}
	if (strstr(http,"HTTP/1.1")) {
		printf("Connection: close\n");
	}
	
	printf("\n");
	printf("*** End of Request ***\n");
	printf("Request %d: Forwarded %d bytes from server to client\n", 
		   idx, bytes_sent);

	// modify log file here with lock()
	pthread_mutex_lock(&log_mutex); 

	log = create_log_entry(clientaddrp, uri, bytes_sent);
	// log = create_log_entry(&req_info->clientaddr, uri, bytes_sent);
	strcat(log, "\n");

	fwrite(log, sizeof(char), strlen(log), log_file);
	fflush(log_file);

	pthread_mutex_unlock(&log_mutex); 

	Free(log);
	Free(host_name);
	Free(port);
	Free(path_name);
	//Free(req_info);
	Free(buffer);

	Close(serverfd);

}



/*
 * Requires:
 *   The parameter "uri" must point to a properly NUL-terminated string.
 *
 * Effects:
 *   Given a URI from an HTTP proxy GET request (i.e., a URL), extract the
 *   host name, port, and path name.  Create strings containing the host name,
 *   port, and path name, and return them through the parameters "hostnamep",
 *   "portp", "pathnamep", respectively.  (The caller must free the memory
 *   storing these strings.)  Return -1 if there are any problems and 0
 *   otherwise.
 */
static int
parse_uri(const char *uri, char **hostnamep, char **portp, char **pathnamep)
{
	const char *pathname_begin, *port_begin, *port_end;

	if (strncasecmp(uri, "http://", 7) != 0)
		return (-1);

	/* Extract the host name. */
	const char *host_begin = uri + 7;
	const char *host_end = strpbrk(host_begin, ":/ \r\n");
	if (host_end == NULL)
		host_end = host_begin + strlen(host_begin);
	int len = host_end - host_begin;
	char *hostname = Malloc(len + 1);
	strncpy(hostname, host_begin, len);
	hostname[len] = '\0';
	*hostnamep = hostname;

	/* Look for a port number.  If none is found, use port 80. */
	if (*host_end == ':') {
		port_begin = host_end + 1;
		port_end = strpbrk(port_begin, "/ \r\n");
		if (port_end == NULL)
			port_end = port_begin + strlen(port_begin);
		len = port_end - port_begin;
	} else {
		port_begin = "80";
		port_end = host_end;
		len = 2;
	}
	char *port = Malloc(len + 1);
	strncpy(port, port_begin, len);
	port[len] = '\0';
	*portp = port;

	/* Extract the path. */
	if (*port_end == '/') {
		pathname_begin = port_end;
		const char *pathname_end = strpbrk(pathname_begin, " \r\n");
		if (pathname_end == NULL)
			pathname_end = pathname_begin + strlen(pathname_begin);
		len = pathname_end - pathname_begin;
	} else {
		pathname_begin = "/";
		len = 1;
	}
	char *pathname = Malloc(len + 1);
	strncpy(pathname, pathname_begin, len);
	pathname[len] = '\0';
	*pathnamep = pathname;

	return (0);
}

/*
 * Requires:
 *   The parameter "sockaddr" must point to a valid sockaddr_in structure.  The
 *   parameter "uri" must point to a properly NUL-terminated string.
 *
 * Effects:
 *   Returns a string containing a properly formatted log entry.  This log
 *   entry is based upon the socket address of the requesting client
 *   ("sockaddr"), the URI from the request ("uri"), and the size in bytes of
 *   the response from the server ("size").
 */
static char *
create_log_entry(const struct sockaddr_in *sockaddr, const char *uri, int size)
{
	struct tm result;

	/*
	 * Create a large enough array of characters to store a log entry.
	 * Although the length of the URI can exceed MAXLINE, the combined
	 * lengths of the other fields and separators cannot.
	 */
	const size_t log_maxlen = MAXLINE + strlen(uri);
	char *const log_str = Malloc(log_maxlen + 1);

	/* Get a formatted time string. */
	time_t now = time(NULL);
	int log_strlen = strftime(log_str, MAXLINE, "%a %d %b %Y %H:%M:%S %Z: ",
	    localtime_r(&now, &result));

	/*
	 * Convert the IP address in network byte order to dotted decimal
	 * form.
	 */
	Inet_ntop(AF_INET, &sockaddr->sin_addr, &log_str[log_strlen],
	    INET_ADDRSTRLEN);
	log_strlen += strlen(&log_str[log_strlen]);

	/*
	 * Assert that the time and IP address fields occupy less than half of
	 * the space that is reserved for the non-URI fields.
	 */
	assert(log_strlen < MAXLINE / 2);

	/*
	 * Add the URI and response size onto the end of the log entry.
	 */
	snprintf(&log_str[log_strlen], log_maxlen - log_strlen, " %s %d", uri,
	    size);

	return (log_str);
}

/*
 * Requires:
 *   The parameter "fd" must be an open socket that is connected to the client.
 *   The parameters "cause", "short_msg", and "long_msg" must point to properly 
 *   NUL-terminated strings that describe the reason why the HTTP transaction
 *   failed.  The string "short_msg" may not exceed 32 characters in length,
 *   and the string "long_msg" may not exceed 80 characters in length.
 *
 * Effects:
 *   Constructs an HTML page describing the reason why the HTTP transaction
 *   failed, and writes an HTTP/1.0 response containing that page as the
 *   content.  The cause appearing in the HTML page is truncated if the
 *   string "cause" exceeds 2048 characters in length.
 */
static void
client_error(int fd, const char *cause, int err_num, const char *short_msg,
    const char *long_msg)
{
	char body[MAXBUF], headers[MAXBUF], truncated_cause[2049];

	assert(strlen(short_msg) <= 32);
	assert(strlen(long_msg) <= 80);
	/* Ensure that "body" is much larger than "truncated_cause". */
	assert(sizeof(truncated_cause) < MAXBUF / 2);

	/*
	 * Create a truncated "cause" string so that the response body will not
	 * exceed MAXBUF.
	 */
	strncpy(truncated_cause, cause, sizeof(truncated_cause) - 1);
	truncated_cause[sizeof(truncated_cause) - 1] = '\0';

	/* Build the HTTP response body. */
	snprintf(body, MAXBUF,
	    "<html><title>Proxy Error</title><body bgcolor=""ffffff"">\r\n"
	    "%d: %s\r\n"
	    "<p>%s: %s\r\n"
	    "<hr><em>The COMP 321 Web proxy</em>\r\n",
	    err_num, short_msg, long_msg, truncated_cause);

	/* Build the HTTP response headers. */
	snprintf(headers, MAXBUF,
	    "HTTP/1.0 %d %s\r\n"
	    "Content-type: text/html\r\n"
	    "Content-length: %d\r\n"
	    "\r\n",
	    err_num, short_msg, (int)strlen(body));

	/* Write the HTTP response. */
	if (rio_writen(fd, headers, strlen(headers)) != -1)
		rio_writen(fd, body, strlen(body));
}

// Prevent "unused function" and "unused variable" warnings.
static const void *dummy_ref[] = { client_error, create_log_entry, dummy_ref,
    parse_uri };



