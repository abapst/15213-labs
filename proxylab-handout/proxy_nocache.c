/* Proxylab for CMU 15-213/513, Fall 2015
 * Author: Aleksander Bapst (abapst)
 * Random port number for abapst: 45318
 *
 * A no-frills multi-threaded proxy server for retrieving web content on behalf
 * of clients. A simple 1 MB cache is used to store objects locally for faster
 * service, with a maximum allowed object size in the cache of 1 KB. Only the
 * GET request is supported, but most http sites can be loaded with this proxy. 
 *
 * Concurrency:
 *     The proxy uses a multi-threaded setup with the cache as the only shared
 *     variable. Semaphores are used to protect the cache from read/write
 *     errors.
 *
 * csapp.c
 *     I modified a few wrapper functions.
 *     Rio_writen - returns a value so that errors can be caught. Also does
 *                  not terminate if errno = EPIPE.
 *     Rio_readn
 *     Rio_readnb
 *     Rio_readlinb - These functions do not terminate if errno = ECONNRESET 
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "csapp.h"

/* Function declarations */
void *client_job(void *connfdp);
int forward_request(int clientfd, int *serverfd);
void close_openfds(int *clientfd, int *serverfd);
int parse_request(char *buf, char *method, char *version,
                  char *protocol, char *hostname, char *filename);
int forward_server_response(int clientfd, int serverfd);

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";
static const char *connection_hdr = "Connection: close\r\n";
static const char *host_hdr_prefix = "Host:";
static const char *proxy_connection_hdr = "Proxy-Connection: close\r\n";
static const char *http_version = "HTTP/1.0\r\n";

/*
 * main - Initializes proxy server and starts listening for requests.
 *        Requests are handled in a multi-threaded manner.
 */
int main(int argc, char **argv)
{
    int listenfd, *connfdp;
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    pthread_t tid;

    /* Ignore SIGPIPE */
    Signal(SIGPIPE, SIG_IGN);

    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    /* Listen for client requests and create new threads when they arrive */
    listenfd = Open_listenfd(argv[1]);
    printf("Proxy server started, listening on port %s\n", argv[1]);
    while (1) {
        clientlen = sizeof(clientaddr);
        connfdp = Malloc(sizeof(int));
        *connfdp = Accept(listenfd, (SA *) &clientaddr, &clientlen);
        Pthread_create(&tid, NULL, client_job, connfdp);
    }
    return 0;
}

/*
 * client_job - Safely handles a client thread in a self-contained manner.
 *              A request is processed and if data is found in the cache,
 *              the cache is instructed to return data to the client.
 *              Otherwise the request is forward to the host. Upon successful
 *              completion of a request, or if something goes wrong, any open 
 *              file descriptors are closed and the thread gracefully exists.
 */
void *client_job(void *connfdp) {
    int clientfd = *((int *)connfdp); 
    int serverfd = -1;
    int request_token, response_token;
    Pthread_detach(Pthread_self());
    Free(connfdp); /* Free allocated connfdp pointer to avoid leak */

    /* Process request. Possible return values are:
     * -1: error, close thread
     *  1: requested object found in cache
     *  2: requested object not found in cache, forward to server
     */
    request_token = forward_request(clientfd, &serverfd);  
    if (request_token < 0) {
        close_openfds(&clientfd, &serverfd);
        Pthread_exit(NULL);
    }

    response_token = forward_server_response(clientfd, serverfd);
    if (response_token < 0) {
        close_openfds(&clientfd, &serverfd);
        Pthread_exit(NULL);
    }

    close_openfds(&clientfd, &serverfd);
    Pthread_exit(NULL);
    return NULL;
}

/*
 * forward_request - Forward a request from a client to the specified server.
 *                   The request line and headers are parsed and recomposed
 *                   into a new request that is passed on to the host. The only
 *                   supported method is GET. If a port number is not supplied,
 *                   the default port of 80 is used. Before forwarding, the
 *                   cache is searched for a matching object. If one is found,
 *                   the data is written back to the client. Otherwise, the
 *                   request is sent forward to the host.
 */
int forward_request(int clientfd, int *serverfd) {
    char buf[MAXLINE], forward_buf[MAXLINE];
    char method[MAXLINE], version[MAXLINE]; 
    char protocol[MAXLINE], hostname[MAXLINE], filename[MAXLINE];
    char host_port[MAXLINE];
    char host_hdr[MAXLINE];
    char *colon;
    rio_t rio_client;

    /* Read the request line from the client */
    Rio_readinitb(&rio_client, clientfd);
    if (!Rio_readlineb(&rio_client, buf, MAXLINE))
        return -1; 

    /* Parse the request line into relevant components */ 
    if (parse_request(buf, method, version,
                      protocol, hostname, filename) < 0)
        return -1; 

    /* Get the requested port, else set it to 80 by default */
    if ((colon = index(hostname, ':')) != NULL) {
        *colon = '\0';    
        strcpy(host_port, colon+1);
    } else {
        strcpy(host_port, "80");
    } 

    printf("Received forwarding request for %s from %s on port %s\n",
           filename, hostname, host_port);
    
    /* Make the host header string */
    strcpy(host_hdr, host_hdr_prefix);
    strcat(host_hdr, " ");
    strcat(host_hdr, hostname);
    strcat(host_hdr, "\r\n");

    /* We only support the GET method */
    if (strcasecmp(method, "GET"))
        return -1;

    /* Compose forwarding request line */
    strcpy(forward_buf, method);
    strcat(forward_buf, " ");
    strcat(forward_buf, filename);
    strcat(forward_buf, " ");
    strcat(forward_buf, http_version);

    /* Read client headers and compose forwarding buffer headers */
    while (Rio_readlineb(&rio_client, buf, MAXLINE)) {
        if (!strcmp(buf, "\r\n")) {
            break;
        } else if (strstr(buf, "User-Agent:") != NULL) {
            strcat(forward_buf, user_agent_hdr); 
        } else if (strstr(buf, "Connection:") != NULL) {
            strcat(forward_buf, connection_hdr);
        } else if (strstr(buf, "Proxy-Connection:") != NULL) {
            strcat(forward_buf, proxy_connection_hdr);
        } else if (strstr(buf, "Host:") != NULL) {
            if (strlen(host_hdr) > 0)
                strcat(forward_buf, host_hdr);        
            else
                strcat(forward_buf, buf);
        } else {
            strcat(forward_buf, buf); /* Forward any other headers */
        }
    }
    strcat(forward_buf, "\r\n"); /* Don't forget to add final line */

    /* Forward request from client to host */
    *serverfd = open_clientfd(hostname, host_port);
    if (*serverfd < 0) {
        return -1;
    }
    if (Rio_writen(*serverfd, forward_buf, strlen(forward_buf)) == -1)
        return -1;
    return 0;
}

/*
 * forward_server_response - Pass a request response from a host back to the
 *                    requesting client. The returned data is also loaded into
 *                    the cache, evicting objects if necessary. The headers and 
 *                    response body are simply written back to the client in 
 *                    buffer lines of size MAXLINE.
 */
int forward_server_response(int clientfd, int serverfd) {
    unsigned int nbytes = 0, obj_size = 0;
    char buf[MAXLINE];
    rio_t rio_server;

    printf("Host response:\n");

    Rio_readinitb(&rio_server, serverfd);
    /* Read the response line from the host */
    if (!Rio_readlineb(&rio_server, buf, MAXLINE))
        return -1; 

    /* Write response line to client */
    if (Rio_writen(clientfd, buf, strlen(buf)) == -1)
        return -1;
    printf("%s",buf);

    /* Read and forward response headers from the host */
    while (strcmp(buf, "\r\n") && strlen(buf) > 0) {
        if (!Rio_readlineb(&rio_server, buf, MAXLINE))
            return -1; 
        if (strstr(buf, "Content-length") != NULL) {
            sscanf(buf, "Content-length: %d", &obj_size);
        }

        /* Write header line to client */
        if (Rio_writen(clientfd, buf, strlen(buf)) == -1)
            return -1;

        printf("%s",buf);
    }

    /* Read and forward response body from the host */
    if (obj_size > 0) {
	while (obj_size > 0) {
	    if (obj_size >= MAXLINE) {
		if ((nbytes = Rio_readnb(&rio_server, buf, MAXLINE)) < 0)
		    return -1; 

                if (Rio_writen(clientfd, buf, nbytes) == -1)
                    return -1;

		obj_size -= MAXLINE;
	    } else {
		if ((nbytes = Rio_readnb(&rio_server, buf, obj_size)) < 0)
		    return -1; 

                if (Rio_writen(clientfd, buf, nbytes) == -1)
                    return -1;

                obj_size = 0;
	    }
	}
    /* If response header had no size line */
    } else {
        while ((nbytes = Rio_readnb(&rio_server, buf, MAXLINE)) > 0) {

            if (Rio_writen(clientfd, buf, nbytes) == -1)
                return -1;
        }
    }

    return 0;
}

/*
 * parse_request - splits a request line into method, url, and version.
 *                 The url is further split into the protocol, hostname,
 *                 and filename (requested resource). Back in the caller,
 *                 the hostname will be further parsed to see if it contains
 *                 a port number.
 */
int parse_request(char *buf, char *method, char *version,
                  char *protocol, char *hostname, char *filename) {

    char url[MAXLINE];

    /* Set requested filename to '/' by default */
    strcpy(filename, "/");

    sscanf(buf, "%s %s %s", method, url, version);
    sscanf(url, "%[^:]://%[^/]%s", protocol, hostname, filename); 
    return 0;
}

/*
 * Safely close client and server descriptors if either are open.
 */
void close_openfds(int *clientfd, int *serverfd) {
    if (*clientfd >= 0)
        Close(*clientfd);
    if (*serverfd >= 0)
        Close(*serverfd);
}
