#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <signal.h>
#include <pthread.h>

#include <vector>
#include <algorithm>

#define ASSERT(cond, msg)\
if(!(cond)) {\
    fprintf(stderr, "ASSERT FAILED [%s:%d]: %s\n", __FILE__, __LINE__, (msg));\
    exit(-1);\
}

#ifdef DEBUG
#define DASSERT(cond, msg)\
if(!(cond)) {\
    fprintf(stderr, "DASSERT FAILED [%s:%d]: %s\n", __FILE__, __LINE__, (msg));\
    exit(-1);\
}

#define DEBUG_PRINT(fmt, ...) printf("DEBUG PRINT [%s:%d]: " fmt, __FILE__, __LINE__, ##__VA_ARGS__)
#else
#define DASSERT(...) {}
#define DEBUG_PRINT(...) {}
#endif


#define MAX_HANDLE      128
#define MAX_BUF_SIZE    0x1000

using namespace std;

struct worker2_params {
    int fd;
    char *data;
    char *host;
};

pthread_mutex_t workers_lock;
vector<pair<int, pthread_t> > workers;

// recv up to max size
bool recvn(size_t *_ret, int fd, void *buf, size_t size) {
    ssize_t ret = recv(fd, buf, size, 0);
    if (ret == -1 || ret == 0)
        return false;
    *_ret = ret;
    return true;
}

bool sendn(int fd, void *buf, size_t size) {
    ssize_t ret = send(fd, buf, size, 0);
    if (ret == -1 || ret != size)
        return false;
    return true;
}

inline bool strncpy_and_movep(char **p, char *buf, size_t *left_size) {
    size_t len = strlen(buf);
    if (len + 1 >= *left_size)
        return false;
    strncpy(*p, buf, len);
    *p += len;
    *left_size -= len;
    return true;
}

bool parse_request(char *_host, char *_ret, char *buf, size_t max_size) {
    char *ret = _ret;
    size_t left_size = max_size;
    char *tmp = buf;
    char *start_line = strsep(&tmp, "\r\n");
    tmp += 1;                           // Bug on strsep
    DEBUG_PRINT("%s\n", start_line);
    size_t start_line_len = strlen(start_line);
    if (!strncpy_and_movep(&ret, start_line, &left_size))
        return false;
    if (!strncpy_and_movep(&ret, "\r\n", &left_size))
        return false;
    // Header
    char *header;
    char *host = NULL;
    while (NULL != (header = strsep(&tmp, "\r\n"))) {
        DEBUG_PRINT("%d: %s\n", strlen(header), header);
        tmp += 1;
        if (strlen(header) == 0) {
            if (!strncpy_and_movep(&ret, "\r\n", &left_size))
                return false;
            break;
        }
        if (strncasecmp(header, "Host:", 5) == 0) {
            char *p = header + 5;
            char *end = header + strlen(header);
            while(*p == ' ') {
                if (p >= end)
                    return false;
                p++;
            }
            host = p;
        }
        if (!strncpy_and_movep(&ret, header, &left_size))
            return false;
        if (!strncpy_and_movep(&ret, "\r\n", &left_size))
            return false;
    }
    if(header == NULL)
        return false;
    char *body = strsep(&tmp, "\r\n");
    if (body == NULL)
        return false;
    if (!strncpy_and_movep(&ret, body, &left_size))
        return false;
    if (host == NULL)
        return false;
    strncpy(_host, host, strlen(host));
    printf("[*] %s (Host: %s)\n", start_line, host);
    DEBUG_PRINT("%s\n", _ret);
    return true;
}

void *worker2_function (void *_data) {
    struct worker2_params *param = (struct worker2_params *) _data;
    struct addrinfo hints, *infoptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = 0;
    hints.ai_flags = AI_CANONNAME | AI_ALL | AI_ADDRCONFIG;
    int result = getaddrinfo(param->host, NULL, &hints, &infoptr);
    int fd;
    size_t size;
    char *buf;
    struct sockaddr_in servAddr;
    if (result)
        goto EXIT;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    memset(&servAddr, 0, sizeof(servAddr));
    servAddr.sin_family = AF_INET;
    servAddr.sin_addr.s_addr = *((uint32_t *) & (((struct sockaddr_in *)infoptr->ai_addr)->sin_addr));
    servAddr.sin_port = htons(80);
    if (connect(fd, (struct sockaddr *) &servAddr, sizeof(servAddr)))
        goto EXIT;
    size = strlen(param->data);
    if (!sendn(fd, param->data, size))
        goto EXIT;
    buf = (char *)malloc(MAX_BUF_SIZE);
    if (!recvn(&size, fd, buf, MAX_BUF_SIZE))
        goto EXIT;
    sendn(param->fd, buf, size);
    close(fd);
    free(buf);
    EXIT:
    free(param);
}

void *worker_function (void *_data) {
    int client_fd = *((int *)_data);
    char *buf = (char *)malloc(MAX_BUF_SIZE);

    while (1) {
        DEBUG_PRINT("waiting for msg...\n");
        size_t ret = 0;
        if (!recvn(&ret, client_fd, buf, MAX_BUF_SIZE))
            goto EXIT;
        // Parse HTTP request
        DEBUG_PRINT("%s\n", buf);
        if (strlen(buf) != ret) {
            DEBUG_PRINT("skipping something wrong...\n");
            continue;
        }
        char *buf_request = (char *)malloc(MAX_BUF_SIZE);
        char *buf_host = (char *) malloc(MAX_BUF_SIZE);
        if (parse_request(buf_host, buf_request, buf, MAX_BUF_SIZE)) {
            struct worker2_params *param = (struct worker2_params *) (malloc(sizeof (struct worker2_params)));
            param->fd = client_fd;
            param->data = buf_request;
            param->host = buf_host;
            pthread_t thread;
            pthread_create(&thread, NULL, worker2_function, (void *) param);
        }
    }
    EXIT:
    DEBUG_PRINT("exit...\n");
    close(client_fd);
}

void intHandler(int dummy) {
    pthread_mutex_lock(&workers_lock);
    void *msg;
    char *buf_body = "Server closed.\n";
    for(auto it = workers.begin(); it != workers.end(); it++) {
        close(it->first);
    }
    pthread_mutex_unlock(&workers_lock);
    printf("[!] closing server...");
    exit(0);
}

int main (int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        fprintf(stderr, "Listens to specified port, performs web proxy\n");
        fprintf(stderr, "(currently https is not supported\n");
        exit(1);
    }
    
    int port = atoi(argv[1]);

    pthread_mutex_init(&workers_lock, NULL);
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT(server_fd != -1, strerror(errno));
    
    int optval = 1;
    ASSERT(setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) != -1, strerror(errno));
    
    struct sockaddr_in server_struct;
    bzero(&server_struct, sizeof(struct sockaddr_in));
    server_struct.sin_family = AF_INET;
    server_struct.sin_port = htons(port);
    server_struct.sin_addr.s_addr = htonl(INADDR_ANY);
    ASSERT(bind(server_fd, (const sockaddr *) &server_struct, sizeof(server_struct)) != -1, strerror(errno));
    
    ASSERT(listen(server_fd, MAX_HANDLE) != -1, strerror(errno));

    printf("[*] listening on port %d\n", port);
    signal(SIGINT, intHandler);

    while (1) {
        struct sockaddr_in client_struct;
        socklen_t client_struct_len = sizeof(client_struct);
        int *client_fd = new int;
        *client_fd = accept(server_fd, (struct sockaddr *) &client_struct, &client_struct_len);
        DASSERT(*client_fd != -1, strerror(errno));
        pthread_mutex_lock(&workers_lock);
        pthread_t thread;
        ASSERT(pthread_create(&thread, NULL, worker_function, (void *) client_fd) == 0, "worker not created");
        workers.push_back(make_pair(*client_fd, thread));
        pthread_mutex_unlock(&workers_lock);
    }

}