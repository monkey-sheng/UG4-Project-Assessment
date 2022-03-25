#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <string.h>

#define PORT 23333  // server port to listen, use this with wrk
#define EPOLL_TIMEOUT 0  // 1ms, is there any reason to wait for a long time?
#define MAX_EVENTS 16  // might do multithreading later with this
#define HASH_SIZE 1024

extern int errno;

struct sockaddr_in address;
int server_socket;

int epoll_fd;
struct epoll_event ev;
struct epoll_event events[MAX_EVENTS];

struct ll_state_node
{
    int key;  // client_fd
    int value;  // fsm state of client
    struct ll_state_node *next;
};

struct ll_buff_write_node
{
    int key;
    int total_bytes;
    int bytes_offset;
    char *buffer;
    struct ll_buff_write_node *next;
};

struct ll_state_node *client_fsm_map[HASH_SIZE];

// this way we wouldn't need to delete linked list node when closing a socket
// at the cost of a tiny bit more memory
void add_socket_to_fsm_map(int socket)
{
    int hash_val = socket % HASH_SIZE;
    struct ll_state_node *client_state = malloc(sizeof(struct ll_state_node));
    *client_state = (struct ll_state_node){.key = socket, .value = 0, .next = NULL};

    // client_fsm_map[hash_val] = client_state;

    if (client_fsm_map[hash_val] == NULL)
    {
        client_fsm_map[hash_val] = client_state;
    }
    else
    {
        struct ll_state_node *curr_ptr = client_fsm_map[hash_val];
        if (curr_ptr->key == socket)
        {
            curr_ptr->value = 0;  // basically a reset, since fd are reused
            return;
        }
        while (curr_ptr->next != NULL)
        {
            if (curr_ptr->next->key == socket)
            {
                curr_ptr->next->value = 0;  // basically a reset, since fd are reused
                return;
            }
            curr_ptr = curr_ptr->next;
        }
        curr_ptr->next = client_state;
    }
}

int get_socket_fsm_state(int socket)
{
    int hash_val = socket % HASH_SIZE;
    struct ll_state_node *curr_ptr = client_fsm_map[hash_val];
    while (curr_ptr->key != socket)
    {
        // printf("get fsm state chasing pointer for %d\n", socket);
        curr_ptr = curr_ptr->next;
    }
    return curr_ptr->value;
}

void set_socket_fsm_state(int socket, int state)
{
    int hash_val = socket % HASH_SIZE;
    struct ll_state_node *curr_ptr = client_fsm_map[hash_val];
    while (curr_ptr->key != socket)
    {
        curr_ptr = curr_ptr->next;
    }
    curr_ptr->value = state;
}


struct ll_buff_write_node *client_buff_write_map[HASH_SIZE];

// add will not only add new, but also replace/update old ones,
// so no need for set or delete really
void add_buff_write_to_map(int socket, struct ll_buff_write_node *buff_write_struct)
{
    int hash_val = socket % HASH_SIZE;

    if (client_buff_write_map[hash_val] == NULL)
    {
        client_buff_write_map[hash_val] = buff_write_struct;
    }
    else
    {
        struct ll_buff_write_node *curr_ptr = client_buff_write_map[hash_val];

        // if same socket, free pointers and replace struct 
        if (curr_ptr->key == socket)
        {
            struct ll_buff_write_node *next_ptr = curr_ptr->next;
            free(curr_ptr->buffer);
            free(curr_ptr);
            client_buff_write_map[hash_val] = buff_write_struct;
            buff_write_struct->next = next_ptr;
            return;
        }
        while (curr_ptr->next != NULL)
        {
            if (curr_ptr->next->key == socket)
            {
                struct ll_buff_write_node *next_ptr = curr_ptr->next->next;
                free(curr_ptr->next->buffer);
                free(curr_ptr->next);
                curr_ptr->next = buff_write_struct;
                buff_write_struct->next = next_ptr;
                return;
            }
            curr_ptr = curr_ptr->next;
        }
        curr_ptr->next = buff_write_struct;
    }
}

struct ll_buff_write_node* get_buff_write(int socket)
{
    int hash_val = socket % HASH_SIZE;
    struct ll_buff_write_node *curr_ptr = client_buff_write_map[hash_val];
    while (curr_ptr->key != socket)
    {
        // printf("get fsm state chasing pointer for %d\n", socket);
        curr_ptr = curr_ptr->next;
    }
    return curr_ptr;
}

// unlike fsm states which need to malloc before add, which calls for the need of a separate set function,
// here the add function alone will be enough for updating struct values, no need for set.


// set non blocking mode for sockets
void set_non_blocking(int socket)
{
    int options = fcntl(socket, F_GETFL);
    fcntl(socket, F_SETFL, options | O_NONBLOCK);
}


char status_and_headers[] =
"HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n";

// send a response with the bare minimum of header
void send_response(int client_fd)
{
    int response_payload_size = 64 + (rand() % 4032); // 64 to 4096 bytes as specified
    int content_length_field_size = floor(log10(response_payload_size)) + 1;
    int status_headers_size = sizeof(status_and_headers) - 2 - 1 + content_length_field_size;
    int total_response_size = response_payload_size + status_headers_size;
    char *response_data = malloc(total_response_size);

    // this should truncate the very last \0 byte,
    snprintf(response_data, status_headers_size+1, status_and_headers,
             response_payload_size);
    memset(response_data + status_headers_size, 'a', response_payload_size);

    int written_bytes = write(client_fd, response_data, total_response_size);
    if (written_bytes < 0)
    {
        int error = errno;
        // As I understand, this means absolutely 0 byte can be sent,
        // otherwise it would write part of it and will return the size it could write
        if (error == EWOULDBLOCK)
        {
            // fprintf(stderr, "EWOULDBLOCK on write, handle this\n");
            written_bytes = 0;  // wrote 0 byte
        }
        else
        {
            fprintf(stderr, "error on write\n%s\n", strerror(error));
            // not sure exactly how to handle the other errors, just close it?
            close(client_fd);
            return;
        }
    }
    if (written_bytes < total_response_size)
    {
        // listen for EPOLLOUT to write what's left
        ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP;
        ev.data.fd = client_fd;
        // add epollout
        if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client_fd, &ev) < 0)
        {
            fprintf(stderr, "failed to mod client to epoll: %s\n", strerror(errno));
        }
        int bytes_to_write = total_response_size - written_bytes;
        struct ll_buff_write_node *buff_write_struct = malloc(sizeof(struct ll_buff_write_node));
        *buff_write_struct = (struct ll_buff_write_node)
        {
            .key = client_fd, .total_bytes = total_response_size,
            .bytes_offset = bytes_to_write, .buffer = response_data, .next = NULL
        };
        add_buff_write_to_map(client_fd, buff_write_struct);
    }

    free(response_data);
}

void process_received_data(int client_fd, char *buffer, ssize_t n)
{
    int s = get_socket_fsm_state(client_fd);

    // int s = 0;
    for (int i = 0; i < n; i++)
    {
        char this_char = buffer[i];
        if (s == 0)
        {
            if (this_char == '\r')
            {
                s = 1; // expecting \n now
            }
        }
        else if (s == 1)
        {
            if (this_char == '\n')
            {
                s = 2; // expecting 2nd \r
            }
            else
                s = 0;
        }
        else if (s == 2)
        {
            if (this_char == '\r')
            {
                s = 3; // expecting final \n
            }
            else
                s = 0;
        }
        else if (s == 3)
        {
            if (this_char == '\n')
            {
                s = 0; // now has \r\n\r\n, should be end of a GET request
                send_response(client_fd);
            }
            else
                s = 0;
        }
    }
    set_socket_fsm_state(client_fd, s);
}

void on_server_socket_event(int event_i)
{
    if (events[event_i].events & EPOLLERR)
    {
        printf("server socket EPOLLERR\n");
    }
    if (events[event_i].events & EPOLLHUP)
    {
        printf("server socket EPOLLHUP\n");
    }
    // if (events[event_i].events & EPOLLIN)
    //     printf("server socket EPOLLIN\n");
    
    socklen_t addressLen = sizeof(address);
    int fd = accept(server_socket, (struct sockaddr *)&address, &addressLen);
    if (fd < 0)
    {
        fprintf(stderr, "accept connection failed\n");
        exit(1);
    }

    set_non_blocking(fd);

    printf("accepted connection from client_fd: %d\n", fd);
    // setup fsm hashmap
    add_socket_to_fsm_map(fd);
    ev.events = EPOLLIN | EPOLLRDHUP; // don't set client as edge triggered
    ev.data.fd = fd;
    // add client to epoll
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0)
    {
        fprintf(stderr, "failed to add client to epoll: %s\n", strerror(errno));
    }
    // else
    //     printf("accepted connection from client_fd%d\n", fd);
}

void on_client_socket_event(int event_i)
{
    if (events[event_i].events & EPOLLRDHUP)
    {
        close(events[event_i].data.fd);
        printf("CLIENT %d EPOLLRDHUP, closed\n", events[event_i].data.fd);
        return;
    }

    // last time could not write everything, continue from associated buff_write
    if (events[event_i].events & EPOLLOUT)
    {
        int client_fd = events[event_i].data.fd;

        struct ll_buff_write_node *buff_write_struct = get_buff_write(client_fd);
        char *buffer_to_write = buff_write_struct->buffer + buff_write_struct->bytes_offset;
        int bytes_to_write = buff_write_struct->total_bytes - buff_write_struct->bytes_offset;
        int written_bytes = write(client_fd, buffer_to_write, bytes_to_write);

        // still not finished writing
        if (written_bytes < bytes_to_write)
        {
            buff_write_struct->bytes_offset += written_bytes;
            return; // finish writing this first before reading and possibly writing more
        }
        // else, wrote everything, revert back, but no need to free the struct ptr really
        else
        {
            // not freeing pointers here, they are getting reused when needed
            ev.events = EPOLLIN | EPOLLRDHUP;
            ev.data.fd = client_fd;
            // removed EPOLLOUT since we have sent everything
            if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client_fd, &ev) < 0)
            {
                fprintf(stderr, "failed to mod client to epoll: %s\n", strerror(errno));
            }
        }
    }

    if (events[event_i].events & EPOLLIN)
    {
        char buffer[256];
        int client_fd = events[event_i].data.fd;

        // read available data
        ssize_t nbytes_read = read(client_fd, buffer, sizeof(buffer));
        if (nbytes_read < 0)
        {
            // I don't think this should happen?
            close(client_fd);
            fprintf(stderr, "error when reading from client, closing\n%s\n", strerror(errno));
        }
        // already listening for EPOLLRDHIP, will not read 0 bytes
        process_received_data(client_fd, buffer, nbytes_read);
    }
}


/*
    Read to the end of an HTTP request, then send response when reading a full request,
    since there is no meaningful processing to be done on the request.
    I'm working under the assumption of simple GET requests - no payload body,
    so basically checking for \r\n\r\n that signifies the end of a request
    I searched for how real web servers parses requests etc, but I don't think I have enough time to go into it,
    and now that I think about it, how is parsing data from a byte stream and forwarding to other applications done in general?
    Anyway here I'm using an FSM just to check the double CRLF of requests.
*/
int main()
{
    printf("preparing listening socket\n");
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    server_socket = socket(AF_INET, SOCK_STREAM, 0);


    if (server_socket == 0)
    {
        perror("server socket failed");
        exit(1);
    }

    set_non_blocking(server_socket);
    
    // set reuse address and port
    int opt = 1;
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT,
                   &opt, sizeof(opt)))
    {
        perror("setsockopt failed");
        exit(1);
    }

    if (bind(server_socket, (struct sockaddr *)&address, sizeof(address)) != 0)
    {
        perror("bind socket failed");
        exit(1);
    }
    if (listen(server_socket, SOMAXCONN) < 0)
    {
        perror("socket listen failed");
        exit(1);
    }
    if ((epoll_fd = epoll_create1(0)) < 0)
    {
        perror("server socket epoll create failed");
        exit(1);
    }
    
    // Configure listening socket for epoll operations
    ev.data.fd = server_socket;
    ev.events = EPOLLIN | EPOLLERR | EPOLLHUP;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_socket, &ev) < 0)
    {
        perror("Failed to configure server epoll fd");
        exit(1);
    }

    printf("main loop starts\n");
    while (1)
    {
        // EPOLL_TIMEOUT, 0 is best throughput it seems?
        int n_events = epoll_wait(epoll_fd, events, MAX_EVENTS, EPOLL_TIMEOUT); 
        if (n_events == -1)
        {
            fprintf(stderr, "epoll_wait() failed to wait for events.");
            exit(1);
        }

        for (int i = 0; i < n_events; i++)
        {
            if (events[i].data.fd == server_socket)
                on_server_socket_event(i);
            else
                on_client_socket_event(i);
        }
    }
}
