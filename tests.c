#define _GNU_SOURCE
#include <sched.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/time.h> 
#include <sys/epoll.h> 
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h> 
#include <pthread.h>
#include <signal.h>


#define MAX_CLIENTS 3000
#if 0
#define EDGE_TRIGGER 0
#else
#define EDGE_TRIGGER EPOLLET
#endif

static void clean_up(int fd);
static void initiate_client_sockets();
static void init_client_socket();
static void print_test_results();
static void register_start_of_test();

int g_seq = 0;

typedef struct
{
    int client_side_port_base;
    int server_side_port_base;
    unsigned int client_ip;
    unsigned int server_ip;
    int buf_size;
    char *buffer; 
    int client_connections_number;
    int writer_epoll_fd; 
    int fds[MAX_CLIENTS];
    int epoll_fd;
    int listener_fd;
    int client_idx;
    int type;
    int rxtx_flag;
}cb_t;

struct timeval start_tv;
int termination_criteria;
uint64_t total_written = 0;
uint64_t total_read = 0;

#define RX_ON 0x1
#define TX_ON 0x2
#define MODE_TCP 1
#define MODE_UDP 2

cb_t *cbs = NULL;
pthread_t *threads;

static void clean_up(int fd);

static void init_server_sock(cb_t *cb)
{
    struct sockaddr *sa;
    int len;
    int val; 
    struct sockaddr_in sockaddrin;

    cb->epoll_fd = epoll_create(10000);

    if(cb->type != MODE_TCP) {
        return;
    }

    sockaddrin.sin_family = AF_INET;
    sockaddrin.sin_port = htons(cb->client_side_port_base);
    sockaddrin.sin_addr.s_addr = cb->server_ip;
    sa = (struct sockaddr *)&sockaddrin;
    len = sizeof(sockaddrin);
    
    cb->listener_fd = socket(AF_INET,SOCK_STREAM,0);
    if(cb->listener_fd <= 0) {
        printf("PANIC: cannot open socket %s %d %d\n",__FILE__,__LINE__,errno);
        exit(2);
    }
    val = 1;
    setsockopt(cb->listener_fd,SOL_SOCKET, SO_REUSEADDR, &val,sizeof(val));
    val = 1;
    if(ioctl(cb->listener_fd,FIONBIO,(char *)&val)) {
        printf("cannot go non-blocking mode\n");
    }
    if(bind(cb->listener_fd,sa,len) < 0) {
        printf("PANIC: cannot bind %s %d %d\n",__FILE__,__LINE__,errno);
        exit(3);
    }
    if(listen(cb->listener_fd,10000) < 0) {
        printf("PANIC: cannot listen %s %d %d\n",__FILE__,__LINE__,errno);
    }
    printf("listener created %d\n",ntohs(sockaddrin.sin_port));
}

static void init_client_socket(cb_t *cb)
{
    struct sockaddr *sa;
    int len;
    int val;
    int fd;
    struct epoll_event new_event;
    struct sockaddr_in sockaddrin;
    int family = (cb->type == 1) ? SOCK_STREAM : SOCK_DGRAM;
    sockaddrin.sin_family = AF_INET;
    sockaddrin.sin_port = htons(cb->server_side_port_base);
    sockaddrin.sin_addr.s_addr = cb->client_ip;
    sa = (struct sockaddr *)&sockaddrin;
    len = sizeof(sockaddrin);

    fd = socket(AF_INET,family,0);
    if(fd <= 0) {
        printf("PANIC: cannot open socket %s %d %d\n",__FILE__,__LINE__,errno);
        exit(2);
    }
    val = 1;
    setsockopt(fd,SOL_SOCKET, SO_REUSEADDR, &val,sizeof(val));
    val = 1;
    if(ioctl(fd,FIONBIO,(char *)&val)) {
        printf("cannot go non-blocking mode\n");
    }
    new_event.events = 0;
    if(cb->type != MODE_TCP) { 
        if(cb->rxtx_flag & RX_ON)
            new_event.events |= EPOLLIN;
         if(cb->rxtx_flag & TX_ON)
            new_event.events |= EPOLLOUT;
         if(bind(fd,sa,len) < 0) {
            printf("PANIC: cannot bind %s %d %d\n",__FILE__,__LINE__,errno);
            exit(3);
         }
    }
    else {
        new_event.events |= EPOLLIN;
        if(cb->rxtx_flag & TX_ON)
            new_event.events |= EPOLLOUT;
    }
    new_event.data.fd =  fd;
    epoll_ctl(cb->epoll_fd,EPOLL_CTL_ADD,fd,&new_event);
    if((cb->type == MODE_TCP)&&(connect(fd,sa,len) < 0)) {
    }
    cb->fds[cb->client_idx++] = fd;
}

static void do_tcp_sock_read(cb_t *cb,int fd)
{
    int rc;

    do
    {
        rc = read(fd,cb->buffer,cb->buf_size);
        if(rc > 0) {
            //printf("read %d\n",rc);
            __sync_fetch_and_add(&total_read,rc);
        }
        else {
            break;
        }
    }while(EDGE_TRIGGER);
}

static void do_udp_sock_read(cb_t *cb,int fd)
{
    int rc,addr_len;
    struct sockaddr *sa;
    struct sockaddr_in sockaddrin;
    sa = (struct sockaddr *)&sockaddrin;
    addr_len = sizeof(sockaddrin);
    do
    {
    	rc = recvfrom(fd,cb->buffer,cb->buf_size,0,sa,&addr_len);
        if(rc > 0) {
            __sync_fetch_and_add(&total_read,rc);
        }
        else {
            break;
        }
    }while(EDGE_TRIGGER);
}

static void do_tcp_sock_write(cb_t *cb,int fd)
{
    int rc;
    sprintf(cb->buffer,"JURA HOY%d",g_seq++);
    do
    {
        rc = write(fd,cb->buffer,cb->buf_size);
        if(rc > 0) {
            __sync_fetch_and_add(&total_written,rc);
        }
        else {
            break;
        }
    }while(EDGE_TRIGGER);
}

static void do_udp_sock_write(cb_t *cb,int fd)
{
    int rc;
    struct sockaddr *sa;
    struct sockaddr_in sockaddrin;
    sprintf(cb->buffer,"JURA HOY%d",g_seq++);
    sockaddrin.sin_family = AF_INET;
    sockaddrin.sin_port = htons(cb->server_side_port_base);
    sockaddrin.sin_addr.s_addr = cb->server_ip;
    sa = (struct sockaddr *)&sockaddrin;
    do
    {
    	rc = sendto(fd,cb->buffer,cb->buf_size,0,sa,sizeof(sockaddrin));
        if(rc > 0) {
            __sync_fetch_and_add(&total_written,rc);
        }
        else {
            break;
        }
    }while(EDGE_TRIGGER);
}

static void do_sock_test_left_side(cb_t *cb)
{
    int i,iterations = 0;

    struct epoll_event events[MAX_CLIENTS+1];   
    struct epoll_event new_event;
    int events_occured,new_sock,len;
    struct sockaddr sa;
 
    if(cb->type == MODE_TCP) {
        events[0].events = EPOLLIN;
        events[0].data.fd = cb->listener_fd;
        epoll_ctl(cb->epoll_fd,EPOLL_CTL_ADD,cb->listener_fd,&events[0]);
    }

    while(1) {       
       events_occured = epoll_wait(cb->epoll_fd,events,MAX_CLIENTS+1,-1);
       for(i = 0;i < events_occured;i++) {
           if(events[i].data.fd == cb->listener_fd) {
               len = sizeof(sa);
               new_sock = accept(cb->listener_fd,&sa,&len);
               if(new_sock <= 0) {
                   printf("a problem in accept %d\n",errno);
                   /*exit(5);*/
               }
               else {
               //    printf("%s %d\n",__FILE__,__LINE__);
               }
               new_event.events = EPOLLIN | EPOLLOUT;
               new_event.data.fd = cb->listener_fd;
               epoll_ctl(cb->epoll_fd,EPOLL_CTL_ADD,cb->listener_fd,&new_event);
               if(new_sock > 0) {
                   new_event.events = EDGE_TRIGGER;
                   if(cb->rxtx_flag & TX_ON) {
                       do_tcp_sock_write(cb,new_sock);
                       new_event.events |= EPOLLOUT;
                   }
                   if(cb->rxtx_flag & RX_ON) {
  	               new_event.events |= EPOLLIN;
                   }
                   new_event.data.fd = new_sock;
                   epoll_ctl(cb->epoll_fd,EPOLL_CTL_ADD,new_sock,&new_event);
               }
           }
           if((cb->listener_fd != events[i].data.fd)&&(events[i].events & EPOLLIN)&&(cb->rxtx_flag & RX_ON)) {
               if(cb->type == MODE_TCP)
                   do_tcp_sock_read(cb,events[i].data.fd);
               else
                   do_udp_sock_read(cb,events[i].data.fd);
           }
           if((cb->listener_fd != events[i].data.fd)&&(events[i].events & EPOLLOUT)&&(cb->rxtx_flag & TX_ON)) {
               if(cb->type == MODE_TCP)
                   do_tcp_sock_write(cb,events[i].data.fd);
               else
                   do_udp_sock_write(cb,events[i].data.fd);
           }
       }
       /*iterations++;
       if(iterations == cb->full_iterations) {
           for(i = 0;i < cb->client_idx;i++) {
               clean_up(cb->fds[i]);
           }
       }*/  
   }
}

static void do_sock_test(cb_t *cb)
{
    do_sock_test_left_side(cb);
}

static void print_test_results()
{
    struct timeval end_tv;
    unsigned int secs;
    unsigned int usecs;

    gettimeofday(&end_tv,NULL);
    secs = end_tv.tv_sec - start_tv.tv_sec;
    usecs = end_tv.tv_usec - start_tv.tv_usec;
    
//    printf("r%u w%u bytes in %d seconds %d usec\n",total_read,total_written,secs,usecs);
    if(secs > 0) {
        printf("tx%"PRIu64" rx%"PRIu64"  bits/sec secs %d total written %"PRIu64" total_read %"PRIu64"\n",(total_written/secs)<<3,(total_read/secs)<<3,secs,total_written,total_read);
    }
}

static void register_start_of_test()
{
    gettimeofday(&start_tv,NULL);
}

static void clean_up(int fd)
{
    shutdown(fd,SHUT_RDWR);
    close(fd);
}

static void init_client_sockets(cb_t *cb)
{
    int i;

    for(i = 0;i < cb->client_connections_number;i++) {
        init_client_socket(cb);
    }
}

void do_test(void *arg)
{  
   cb_t *cb = (cb_t *)arg;
   sigset_t sigpipe_mask;
   sigemptyset(&sigpipe_mask);
   sigaddset(&sigpipe_mask, SIGPIPE);
   sigset_t saved_mask;
   if (pthread_sigmask(SIG_BLOCK, &sigpipe_mask, &saved_mask) == -1) {
      perror("pthread_sigmask");
      exit(1);
   }
   init_server_sock(cb);
   init_client_sockets(cb);
   register_start_of_test(cb);
   do_sock_test(cb);
}

void init_test(int buf_sz,
               int number_of_threads,
               unsigned int termination_crit,
               int clnt_conn_num,
               int clnt_side_pb,
               int srv_side_pb,
	       	   unsigned int clnt_ip,
		       unsigned int srv_ip,
               int type,
               int rxtx_flag)
{
    int idx;
    cpu_set_t cpuset;

    cbs = (cb_t *)malloc(sizeof(cb_t)*number_of_threads);
    if(!cbs) {
        printf("cannot allocate cbs\n");
        exit(0);
    }
    threads = malloc(sizeof(pthread_t)*number_of_threads);
    if(!threads) {
        printf("Cannot allocate threads\n");
    }
    termination_criteria = termination_crit;
    for(idx = 0;idx < number_of_threads;idx++) {
        memset(&cbs[idx],0,sizeof(cb_t));
        cbs[idx].client_side_port_base = clnt_side_pb+idx;
        cbs[idx].server_side_port_base = srv_side_pb+idx;
        printf("server side port %d client side port %d\n",cbs[idx].server_side_port_base,cbs[idx].client_side_port_base);
        cbs[idx].buf_size = buf_sz; 
        cbs[idx].client_connections_number = clnt_conn_num;
        cbs[idx].client_ip = clnt_ip;
        cbs[idx].server_ip = srv_ip;
        cbs[idx].type = type;
        cbs[idx].rxtx_flag = rxtx_flag;
        cbs[idx].buffer = (char *)malloc(cbs[idx].buf_size);
        if(cbs[idx].buffer == NULL) {
            printf("memory allocation failure %s %d\n",__FILE__,__LINE__);
            exit(0);
        }
        memset(cbs[idx].buffer,0xEE,cbs[idx].buf_size);
        if(pthread_create(&threads[idx],NULL,do_test,(void *)&cbs[idx])) {
            printf("cannot create thread\n");
        }
        CPU_ZERO(&cpuset);
        CPU_SET(idx, &cpuset); 
        if (pthread_setaffinity_np(threads[idx], sizeof(cpu_set_t), &cpuset) != 0)
               printf("error in pthread_setaffinity_np\n");
    }
}

int main(int argc, char **argv)
{
    int rxtx = RX_ON|TX_ON;
    int duration = 60;
    int i;
    if(argc < 9)
    {
        printf("Usage:  <buf_size> <number_of_threads> <bytes rx/tx to print stats> <client conn num> <client port base> <server port base> <connectip> <acceptip> <type (1-tcp,2-udp> [rxtx (0x1 - write, 0x2 - read)]\n");
        exit(1);
    }
    if(argc == 11) {
        rxtx = atoi(argv[10]);
    }
    printf("Entered: buf_size %d thread_number %d bytes rx/tx to print stats %d client conn num %d client port base %d server port base %d connectip %s acceptip %s family %d mode %d\n",
           atoi(argv[1]),atoi(argv[2]),atoi(argv[3]),atoi(argv[4]),atoi(argv[5]),atoi(argv[6]),argv[7],argv[8],atoi(argv[9]),rxtx);
    system("cat /proc/interrupts");
    init_test(atoi(argv[1]),atoi(argv[2]),atoi(argv[3]),atoi(argv[4]),atoi(argv[5]),atoi(argv[6]),inet_addr(argv[7]),inet_addr(argv[8]),atoi(argv[9]),rxtx);
    register_start_of_test();
    while(i < duration) {
        sleep(1);
         if((total_written >= termination_criteria)||(total_read >= termination_criteria)) {
	      print_test_results(); 
       }
       i++;
    }
    system("cat /proc/interrupts");
    return 0;
}
