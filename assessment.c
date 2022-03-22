#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <fcntl.h>

#define PORT 114514  // server port to listen, use this with wrk
#define EPOLL_TIMEOUT 1  // 1ms, is there any reason to wait for a long time?
#define MAX_EVENTS 16  // might do multithreading later with this

int main()
{
    printf("hello world");
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);
    
    int server_socket = socket(AF_INET, SOCK_STREAM, 0);

    int epoll_fd;
    struct epoll_event ev;
    struct epoll_event events[MAX_EVENTS];

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
    
    // Configure listening socket for all epoll operations
    ev.data.fd = server_socket;
    ev.events = EPOLLIN | EPOLLPRI | EPOLLERR | EPOLLHUP | EPOLLET;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_socket, &ev) < 0)
    {
        perror("Failed to configure server epoll fd");
        exit(1);
    }

    while (1)
    {
        int n_events = epoll_wait(epoll_fd, events, MAX_EVENTS, EPOLL_TIMEOUT);
        if (n_events == -1)
        {
            perror("epoll_wait() failed to wait for events.");
            exit(1);
        }

        for (int i = 0; i < n_events; i++)
        {
            if (events[i].data.fd == server_socket)
            // TODO
                handleListeningFileDescriptorActivity();
            else
            // TODO
                handleClientFileDescriptorActivity(i);
        }
    }
}

// set non blocking mode for socket
void set_non_blocking(int socket)
{

    int options = fcntl(socket, F_GETFL);
    fcntl(socket, F_SETFL, options | O_NONBLOCK);
}
