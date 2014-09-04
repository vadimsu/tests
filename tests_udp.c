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

int epoll_fd = 0;
char *buffer = NULL;
int buffer_size = 0;
unsigned int total_read = 0;
unsigned int total_written = 0;
unsigned int termination_criteria = 0;
unsigned short port_base = 0;
unsigned short remote_port_base = 0;
unsigned int ip2bind = 0;
unsigned int remote_ip = 0;
int connections_number = 0;
struct timeval start_tv;
struct timeval end_tv;
#define MAX_CLIENTS 30000
#if 1
#define EDGE_TRIGGER 0
#else
#define EDGE_TRIGGER EPOLLET
#endif

int g_seq = 0;

static void print_test_results();
static void register_start_of_test();
static void register_end_of_test();

static void init_client_socket(unsigned short port)
{
    struct sockaddr *sa;
    struct sockaddr_in sockaddrin;
    int len;
    int val;
    int new_fd;
    struct epoll_event new_event;

    sockaddrin.sin_family = AF_INET;
    sockaddrin.sin_port = port;
    sockaddrin.sin_addr.s_addr = ip2bind;
    printf("IP2BIND %x\n",ip2bind);
    sa = (struct sockaddr *)&sockaddrin;

    new_fd = socket(AF_INET,SOCK_DGRAM,0);
    if(new_fd <= 0)
    {
        printf("PANIC: cannot open socket %s %d %d\n",__FILE__,__LINE__,errno);
        exit(2);
    }
    val = 1;
    setsockopt(new_fd,SOL_SOCKET, SO_REUSEADDR, &val,sizeof(val));
    val = 1;
    if(ioctl(new_fd,FIONBIO,(char *)&val))
    {
        printf("cannot go non-blocking mode\n");
    }
    if(bind(new_fd,sa,sizeof(sockaddrin)) < 0)
    {
    	printf("PANIC: cannot bind %s %d %d\n",__FILE__,__LINE__,errno);
    	exit(3);
    }
    new_event.events = EPOLLIN | EPOLLOUT;
    new_event.data.fd =  new_fd;
    epoll_ctl(epoll_fd,EPOLL_CTL_ADD,new_fd,&new_event);
}

static void do_sock_read(int fd)
{
    int rc,addr_len;
    struct sockaddr *sa;
    struct sockaddr_in sockaddrin;
    sa = (struct sockaddr *)&sockaddrin;
    addr_len = sizeof(sockaddrin);
    do
    {
    	rc = recvfrom(fd,buffer,buffer_size,0,sa,&addr_len);
        if(rc > 0)
        {
            //printf("read %d\n",rc);
            total_read += rc;
        }
    }while(EDGE_TRIGGER);
}

static void do_sock_write(int fd)
{
    int rc;
    struct sockaddr *sa;
    struct sockaddr_in sockaddrin;
    sprintf(buffer,"JURA HOY%d",g_seq++);
    sockaddrin.sin_family = AF_INET;
    sockaddrin.sin_port = htons(remote_port_base);
    sockaddrin.sin_addr.s_addr = remote_ip;
    sa = (struct sockaddr *)&sockaddrin;
    do
    {
    	rc = sendto(fd,buffer,buffer_size,0,sa,sizeof(sockaddrin));
        if(rc > 0)
        {
            //printf("written %d\n",rc);
            total_written += rc;
        }
    }while(EDGE_TRIGGER);
}

static void do_sock_test_left_side()
{
    int i;

    struct epoll_event events[MAX_CLIENTS+1];   
    int events_occured;
 
    while(1)
    {       
       events_occured = epoll_wait(epoll_fd,events,MAX_CLIENTS+1,-1);
       for(i = 0;i < events_occured;i++)
       {
           if(events[i].events & EPOLLIN)
           {
               do_sock_read(events[i].data.fd);
	           //new_event.events = EPOLLIN | EPOLLOUT | EDGE_TRIGGER;
               //new_event.data.fd = events[i].data.fd;
               //epoll_ctl(SOCK_EPOLL_DESCR(cb),EPOLL_CTL_ADD,events[i].data.fd,&new_event);
           }
           if(events[i].events & EPOLLOUT)
           {
               do_sock_write(events[i].data.fd);
	           //new_event.events = EPOLLIN | EPOLLOUT | EDGE_TRIGGER;
               //new_event.data.fd = events[i].data.fd;
               //epoll_ctl(SOCK_EPOLL_DESCR(cb),EPOLL_CTL_ADD,events[i].data.fd,&new_event);
           }
           else
           {
               //printf("%s %d\n",__FILE__,__LINE__);
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
    printf("r%u w%u bytes in %d seconds %d usec\n",total_read,total_written,secs,usecs);
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

    for(i = 0;i < connections_number;i++)
    {
	    printf("init client socket %d\n",i);
        init_client_socket(port_base+i);
    }
}

void do_test()
{
   buffer = (char *)malloc(buffer_size);
   if(buffer == NULL)
   {
        printf("memory allocation failure %s %d\n",__FILE__,__LINE__);
        exit(0);
   }
   memset(buffer,0xEE,buffer_size);
   init_client_sockets();
   register_start_of_test();
   do_sock_test();
   register_end_of_test();
   print_test_results();
}

void init_test(int buf_size,
               unsigned int term_criteria,
               int conn_number,
               int pbase,
               int remote_pbase,
               unsigned int ip,
               unsigned int rmt_ip)
{
    epoll_fd = epoll_create(10000);
    port_base = htons(pbase);
    buffer_size = buf_size;
    termination_criteria = term_criteria;
    connections_number = conn_number;
    port_base = pbase;
    remote_port_base = remote_pbase;
    remote_ip = rmt_ip;
    ip2bind = ip;
}

int main(int argc, char **argv)
{
    if(argc != 8)
    {
        printf("Usage:  <buf_size> <bytes rx/tx to print stats> <conn num> <port base> <remote port base> <my ip> <peer ip>\n");
        exit(1);
    }
    printf("Entered: buf_size %d bytes rx/tx to print stats %d conn num %d port base %d remote port base %d my ip %x ip 2 connect %x \n",
           atoi(argv[1]),atoi(argv[2]),atoi(argv[3]),atoi(argv[4]),atoi(argv[5]),inet_addr(argv[6]),inet_addr(argv[7]));
    init_test(atoi(argv[1]),atoi(argv[2]),atoi(argv[3]),atoi(argv[4]),atoi(argv[5]),inet_addr(argv[6]),inet_addr(argv[7]));
    do_test();
    return 0;
}
