#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/time.h> 
#include <sys/epoll.h> 
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h> 
#include <pthread.h>

#define MAX_CLIENTS 3000
#if 0
#define EDGE_TRIGGER 0
#else
#define EDGE_TRIGGER EPOLLET
#endif

int g_seq = 0;

int instance;
int client_side_port_base;
int server_side_port_base;
unsigned int client_ip;
unsigned int server_ip;
int buf_size = 0;
char *buffer = NULL;
int termination_criteria;
int client_connections_number;
int writer_epoll_fd;
struct timeval start_tv;
struct timeval end_tv;
unsigned int total_written;
unsigned int total_read;
int fds[MAX_CLIENTS];

static void clean_up(int fd);
static void initiate_client_sockets();
static void init_client_socket();
static void print_test_results();
static void register_start_of_test();
static void register_end_of_test();

int epoll_fd = 0;
int listener_fd = 0;
int client_idx = 0;
int full_iterations = 0;
static void clean_up(int fd);

static void init_server_sock()
{
    struct sockaddr *sa;
    int len;
    int val; 
    struct sockaddr_in sockaddrin;

    sockaddrin.sin_family = AF_INET;
    sockaddrin.sin_port = client_side_port_base + instance;
    sockaddrin.sin_addr.s_addr = server_ip;
    sa = &sockaddrin;
    len = sizeof(sockaddrin);
    
    listener_fd = socket(AF_INET,SOCK_STREAM,0);
    if(listener_fd <= 0)
    {
        printf("PANIC: cannot open socket %s %d %d\n",__FILE__,__LINE__,errno);
        exit(2);
    }
    val = 1;
    setsockopt(listener_fd,SOL_SOCKET, SO_REUSEADDR, &val,sizeof(val));
    val = 1;
    if(ioctl(listener_fd,FIONBIO,(char *)&val))
    {
        printf("cannot go non-blocking mode\n");
    }
    if(bind(listener_fd,sa,len) < 0)
    {
        printf("PANIC: cannot bind %s %d %d\n",__FILE__,__LINE__,errno);
        exit(3);
    }
    if(listen(listener_fd,10000) < 0)
    {
        printf("PANIC: cannot listen %s %d %d\n",__FILE__,__LINE__,errno);
    }
    epoll_fd = epoll_create(10000);
}

static void init_client_socket()
{
    struct sockaddr *sa;
    int len;
    int val;
    int fd;
    struct epoll_event new_event;
    struct sockaddr_in sockaddrin;

    sockaddrin.sin_family = AF_INET;
    sockaddrin.sin_port = server_side_port_base + instance;
    sockaddrin.sin_addr.s_addr = client_ip;
    sa = &sockaddrin;
    len = sizeof(sockaddrin);

    fd = socket(AF_INET,SOCK_STREAM,0);
    if(fd <= 0)
    {
        printf("PANIC: cannot open socket %s %d %d\n",__FILE__,__LINE__,errno);
        exit(2);
    }
    val = 1;
    setsockopt(fd,SOL_SOCKET, SO_REUSEADDR, &val,sizeof(val));
    val = 1;
    if(ioctl(fd,FIONBIO,(char *)&val))
    {
        printf("cannot go non-blocking mode\n");
    }
    new_event.events = EPOLLIN | EPOLLOUT;
    new_event.data.fd =  fd;
    epoll_ctl(epoll_fd,EPOLL_CTL_ADD,fd,&new_event);
    //printf("%s %d\n",__FILE__,__LINE__);
    if(connect(fd,sa,len) < 0)
    {
    }
    fds[client_idx++] = fd;
}

static void do_sock_read(int fd)
{
    int rc;

    do
    {
        rc = read(fd,buffer,buf_size);
        if(rc > 0)
        {
            //printf("read %d\n",rc);
            total_read += rc;
        }
        else {
            break;
        }
    }while(EDGE_TRIGGER);
}

static void do_sock_write(int fd)
{
    int rc;
    sprintf(buffer,"JURA HOY%d",g_seq++);
    do
    {
        rc = write(fd,buffer,buf_size);
        if(rc > 0)
        {
            //printf("written %d\n",rc);
            total_written += rc;
        }
        else {
            break;
        }
    }while(EDGE_TRIGGER);
}

static void do_sock_test_left_side()
{
    int i,iterations = 0;

    struct epoll_event events[MAX_CLIENTS+1];   
    struct epoll_event new_event;
    int events_occured,new_sock,len;
    struct sockaddr sa;
 
    events[0].events = EPOLLIN | EPOLLOUT;
    events[0].data.fd = listener_fd;
    epoll_ctl(epoll_fd,EPOLL_CTL_ADD,listener_fd,&events[0]);

    while(1)
    {       
       events_occured = epoll_wait(epoll_fd,events,MAX_CLIENTS+1,-1);
       for(i = 0;i < events_occured;i++)
       {
           if(events[i].data.fd == listener_fd)
           {
               len = sizeof(sa);
               new_sock = accept(listener_fd,&sa,&len);
               if(new_sock <= 0)
               {
                   printf("a problem in accept %d\n",errno);
                   /*exit(5);*/
               }
               else
               {
                   printf("%s %d\n",__FILE__,__LINE__);
               }
               new_event.events = EPOLLIN | EPOLLOUT;
               new_event.data.fd = listener_fd;
               epoll_ctl(epoll_fd,EPOLL_CTL_ADD,listener_fd,&new_event);
               if(new_sock > 0)
               {
                   do_sock_write(new_sock);
		           new_event.events = EPOLLIN | EPOLLOUT | EDGE_TRIGGER;
                   new_event.data.fd = new_sock;
                   epoll_ctl(epoll_fd,EPOLL_CTL_ADD,new_sock,&new_event);
               }
           }
           else if(events[i].events & EPOLLIN)
           {
               do_sock_read(events[i].data.fd);
           }
           else if(events[i].events & EPOLLOUT)
           {
               do_sock_write(events[i].data.fd);
           }
           else
           {
               printf("%s %d\n",__FILE__,__LINE__);
           }
       }
       if((total_written >= termination_criteria)||(total_read >= termination_criteria))
       {
//            iterate_active_fds(cb,clean_up);
//            clean_up(cb,SOCK_FDESCR(cb));
//            break;
	      register_end_of_test();
	      print_test_results();
	      total_written = 0;
	      total_read = 0;
	      register_start_of_test();
              iterations++;
              if(iterations == full_iterations) {
                  for(i = 0;i < client_idx;i++) {
                      clean_up(fds[i]);
                  }
              }
       }  
   }
}

static void do_sock_test()
{
    do_sock_test_left_side();
}

static void print_test_results()
{
    unsigned int secs = end_tv.tv_sec - start_tv.tv_sec;
    unsigned int usecs = end_tv.tv_usec - start_tv.tv_usec;
    printf("r%u w%u bytes in %d seconds %d usec client_side_port_base %d\n",total_read,total_written,secs,usecs,client_side_port_base);
    if(secs > 0)
    {
        printf("which is w%u r%u  bytes/sec\n",total_written/secs,total_read/secs);
    }
}

static void register_start_of_test()
{
    gettimeofday(&start_tv,NULL);
}

static void register_end_of_test()
{
    gettimeofday(&end_tv,NULL);
}

static void clean_up(int fd)
{
    shutdown(fd,SHUT_RDWR);
    close(fd);
}

static void init_client_sockets()
{
    int i;

    for(i = 0;i < client_connections_number;i++)
    {
//	printf("init client socket %d\n",i);
        init_client_socket();
    }
}

void do_test()
{
   buffer = (char *)malloc(buf_size);
   if(buffer == NULL)
   {
        printf("memory allocation failure %s %d\n",__FILE__,__LINE__);
        exit(0);
   }
   memset(buffer,0xEE,buf_size);
   init_server_sock();
   init_client_sockets();
   register_start_of_test();
   do_sock_test();
   register_end_of_test();
   print_test_results();
}

void init_test(int buf_sz,
               int instance_id,
               unsigned int termination_crit,
               int clnt_conn_num,
               int clnt_side_pb,
               int srv_side_pb,
	       	   unsigned int clnt_ip,
		       unsigned int srv_ip,
               int iterations)
{
    client_side_port_base = htons(clnt_side_pb);
    server_side_port_base = htons(srv_side_pb);
    buf_size = buf_sz;
    instance = instance_id;
    termination_criteria = termination_crit;
    client_connections_number = clnt_conn_num;
    client_ip = clnt_ip;
    server_ip = srv_ip;
    full_iterations = iterations;
}

int main(int argc, char **argv)
{
    int iterations = 0;
    if(argc < 9)
    {
        printf("Usage:  <buf_size> <instance_id> <bytes rx/tx to print stats> <client conn num> <client port base> <server port base> <connectip> <acceptip>\n");
        exit(1);
    }
    if(argc == 10) {
        iterations = atoi(argv[9]);
    }
    printf("Entered: buf_size %d instance_id %d bytes rx/tx to print stats %d client conn num %d client port base %d server port base %d\n",
           atoi(argv[1]),atoi(argv[2]),atoi(argv[3]),atoi(argv[4]),atoi(argv[5]),atoi(argv[6]));
    init_test(atoi(argv[1]),atoi(argv[2]),atoi(argv[3]),atoi(argv[4]),atoi(argv[5]),atoi(argv[6]),inet_addr(argv[7]),inet_addr(argv[8]),iterations);
    do_test();
    return 0;
}
