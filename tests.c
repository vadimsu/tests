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

#define MAX_CLIENTS 30000
#if 1
#define EDGE_TRIGGER 0
#else
#define EDGE_TRIGGER EPOLLET
#endif

int g_seq = 0;

typedef struct
{
    int  data;
    int prev;
    int next;
}ll_node_t;

typedef struct
{
    int sock_fd;/* server only */
    int epoll_fd;
    ll_node_t fds[MAX_CLIENTS];
    int free_fds_list_head;
    int active_fds_list_head;
    char *buffer;
    struct sockaddr_in tcp_sock;
}sock_cb_t;


typedef struct
{
    int instance;
    int client_side_port_base;
    int server_side_port_base;
    unsigned int client_ip;
    unsigned int server_ip;
    char client_side_name_base[1024];
    char server_side_name_base[1024];
    int buf_size;
    int termination_criteria;
    int client_connections_number;
    pthread_t writer; 
    int writer_epoll_fd;
    sock_cb_t client_specific_data;
    sock_cb_t server_specific_data;
    struct timeval start_tv;
    struct timeval end_tv;
    unsigned int total_written;
    unsigned int total_read;
}my_ipc_cb_t;

#define SOCK_CLIENT_CB(s) (s->client_specific_data)
#define SOCK_SERVER_CB(s) (s->server_specific_data)
#define SOCK_FDESCR(s) (SOCK_SERVER_CB(s).sock_fd)
#define SOCK_EPOLL_DESCR(s) (SOCK_SERVER_CB(s).epoll_fd)
#define SOCK_CLIENT_SPECIFIC(s) (SOCK_CLIENT_CB(s))
#define SOCK_SERVER_SPECIFIC(s) (SOCK_SERVER_CB(s))

static void clean_up(my_ipc_cb_t *cb,int fd);
static int get_fd(my_ipc_cb_t *cb,int *p_head);
static void put_fd(my_ipc_cb_t *cb,int idx,int *p_head);
static void initiate_client_sockets(my_ipc_cb_t *cb);
static void init_client_socket(my_ipc_cb_t *cb);
static void right_side_writer_thread_proc(void *arg);
static void print_test_results(my_ipc_cb_t *cb);
static void register_start_of_test(my_ipc_cb_t *cb);
static void register_end_of_test(my_ipc_cb_t *cb);

static struct sockaddr *get_sock_addr(my_ipc_cb_t *cb,int *p_sock_addr_len,int is_server_socket)
{
    struct sockaddr *sa;
    char temp[2048];

    if(!is_server_socket)
    {
       	SOCK_CLIENT_SPECIFIC(cb).tcp_sock.sin_family = AF_INET;
        SOCK_CLIENT_SPECIFIC(cb).tcp_sock.sin_port = cb->server_side_port_base + cb->instance;
        SOCK_CLIENT_SPECIFIC(cb).tcp_sock.sin_addr.s_addr = cb->client_ip;
        *p_sock_addr_len = sizeof(SOCK_CLIENT_SPECIFIC(cb).tcp_sock);
        return &(SOCK_CLIENT_SPECIFIC(cb).tcp_sock);
    }
   	SOCK_SERVER_SPECIFIC(cb).tcp_sock.sin_family = AF_INET;
    SOCK_SERVER_SPECIFIC(cb).tcp_sock.sin_port = cb->client_side_port_base + cb->instance;
    SOCK_SERVER_SPECIFIC(cb).tcp_sock.sin_addr.s_addr = cb->server_ip;
    *p_sock_addr_len = sizeof(SOCK_SERVER_SPECIFIC(cb).tcp_sock);
    return &(SOCK_SERVER_SPECIFIC(cb).tcp_sock);
}

static int get_address_family(my_ipc_cb_t *cb,int is_server_socket)
{
  	if(!is_server_socket)
   	{
  		return SOCK_CLIENT_SPECIFIC(cb).tcp_sock.sin_family;
  	}
	return SOCK_SERVER_SPECIFIC(cb).tcp_sock.sin_family;
}

static void init_server_sock(my_ipc_cb_t *cb)
{
    struct sockaddr *sa;
    int len;
    int val; 
    
    sa = get_sock_addr(cb,&len,1);
    if(sa == NULL)
    {
        printf("PANIC: cannot initiate socket %s %d %d\n",__FILE__,__LINE__,errno);
        exit(1);
    }
    SOCK_FDESCR(cb) = socket(get_address_family(cb,1),SOCK_STREAM,0);
    if(SOCK_FDESCR(cb) <= 0)
    {
        printf("PANIC: cannot open socket %s %d %d\n",__FILE__,__LINE__,errno);
        exit(2);
    }
    val = 1;
    setsockopt(SOCK_FDESCR(cb),SOL_SOCKET, SO_REUSEADDR, &val,sizeof(val));
    val = 1;
    if(ioctl(SOCK_FDESCR(cb),FIONBIO,(char *)&val))
    {
        printf("cannot go non-blocking mode\n");
    }
    if(bind(SOCK_FDESCR(cb),sa,len) < 0)
    {
        printf("PANIC: cannot bind %s %d %d\n",__FILE__,__LINE__,errno);
        exit(3);
    }
    if(listen(SOCK_FDESCR(cb),10000) < 0)
    {
        printf("PANIC: cannot listen %s %d %d\n",__FILE__,__LINE__,errno);
    }
    SOCK_EPOLL_DESCR(cb) = epoll_create(10000);
}

static void init_client_socket(my_ipc_cb_t *cb)
{
    struct sockaddr *sa;
    int len;
    int val;
    int new_fd_idx;
    struct epoll_event new_event;

    new_fd_idx = get_fd(cb,&(SOCK_CLIENT_CB(cb).free_fds_list_head));
    if(new_fd_idx == MAX_CLIENTS)
    {
        printf("cannot allocate fd\n");
        exit(0);
        return;
    }
    put_fd(cb,new_fd_idx,&(SOCK_CLIENT_CB(cb).active_fds_list_head));
    sa = get_sock_addr(cb,&len,0);
    if(sa == NULL)
    {
        printf("PANIC: cannot initiate socket %s %d %d\n",__FILE__,__LINE__,errno);
        exit(1);
    }
    SOCK_CLIENT_CB(cb).fds[new_fd_idx].data = socket(get_address_family(cb,0),SOCK_STREAM,0);
    if(SOCK_CLIENT_CB(cb).fds[new_fd_idx].data <= 0)
    {
        printf("PANIC: cannot open socket %s %d %d\n",__FILE__,__LINE__,errno);
        exit(2);
    }
    val = 1;
    setsockopt(SOCK_CLIENT_CB(cb).fds[new_fd_idx].data,SOL_SOCKET, SO_REUSEADDR, &val,sizeof(val));
    val = 1;
    if(ioctl(SOCK_CLIENT_CB(cb).fds[new_fd_idx].data,FIONBIO,(char *)&val))
    {
        printf("cannot go non-blocking mode\n");
    }
    new_event.events = EPOLLIN | EPOLLOUT;
    new_event.data.fd =  SOCK_CLIENT_CB(cb).fds[new_fd_idx].data;
    epoll_ctl(SOCK_EPOLL_DESCR(cb),EPOLL_CTL_ADD,SOCK_CLIENT_CB(cb).fds[new_fd_idx].data,&new_event);
    //printf("%s %d\n",__FILE__,__LINE__);
    if(connect(SOCK_CLIENT_CB(cb).fds[new_fd_idx].data,sa,len) < 0)
    {
    }
}

static void init_free_fds(my_ipc_cb_t *cb)
{
    int i;

    for(i = 0;i < MAX_CLIENTS;i++)
    {
        SOCK_CLIENT_CB(cb).fds[i].data = -1;
        if(i == (MAX_CLIENTS - 1))
        {
            SOCK_CLIENT_CB(cb).fds[i].next = 0;
        }
        else
        {
            SOCK_CLIENT_CB(cb).fds[i].next = i + 1;
        }
        if(i == 0)
        {
            SOCK_CLIENT_CB(cb).fds[i].prev = MAX_CLIENTS - 1;
        }
        else
        {
            SOCK_CLIENT_CB(cb).fds[i].prev = i - 1;
        }
    }
    SOCK_CLIENT_CB(cb).free_fds_list_head = 0;
    SOCK_CLIENT_CB(cb).active_fds_list_head = MAX_CLIENTS;
}

static void iterate_active_fds(my_ipc_cb_t *cb, int (*callback_fnc)(void *,void *))
{
    int i,rc;

    for(i = SOCK_CLIENT_CB(cb).active_fds_list_head;(i != MAX_CLIENTS);i = SOCK_CLIENT_CB(cb).fds[i].next)
    {
        rc = callback_fnc(cb,&(SOCK_CLIENT_CB(cb).fds[i]));
        if(rc)
        {
            break;
        }
    }
}

static int get_fd(my_ipc_cb_t *cb,int *p_head)
{
    int curr;
    
    curr = *p_head;
    if(curr == MAX_CLIENTS)
    {
        return MAX_CLIENTS;
    }
    if(SOCK_CLIENT_CB(cb).fds[curr].next == curr)
    {
        *p_head = MAX_CLIENTS;
    }
    else
    {
    	*p_head = SOCK_CLIENT_CB(cb).fds[curr].next;
        printf("%s %d %d %d %d %d %d %d\n",
              __FILE__,__LINE__,
              curr,
              SOCK_CLIENT_CB(cb).fds[curr].prev,
              SOCK_CLIENT_CB(cb).fds[SOCK_CLIENT_CB(cb).fds[curr].prev].next,
              SOCK_CLIENT_CB(cb).fds[curr].next,
              SOCK_CLIENT_CB(cb).fds[SOCK_CLIENT_CB(cb).fds[curr].next].prev,
              SOCK_CLIENT_CB(cb).fds[curr].prev);
        SOCK_CLIENT_CB(cb).fds[SOCK_CLIENT_CB(cb).fds[curr].prev].next = SOCK_CLIENT_CB(cb).fds[curr].next;
        SOCK_CLIENT_CB(cb).fds[SOCK_CLIENT_CB(cb).fds[curr].next].prev = SOCK_CLIENT_CB(cb).fds[curr].prev;
    }
    return curr; 
}

static void put_fd(my_ipc_cb_t *cb,int idx,int *p_head)
{
    int curr;
    
    curr = *p_head;
    if(curr == MAX_CLIENTS)
    {
        SOCK_CLIENT_CB(cb).fds[idx].next = idx;
        SOCK_CLIENT_CB(cb).fds[idx].prev = idx;
        *p_head = idx;
        return;
    }
    SOCK_CLIENT_CB(cb).fds[idx].next = curr;
    SOCK_CLIENT_CB(cb).fds[idx].prev = SOCK_CLIENT_CB(cb).fds[curr].prev;
    SOCK_CLIENT_CB(cb).fds[SOCK_CLIENT_CB(cb).fds[curr].prev].next = idx;
    SOCK_CLIENT_CB(cb).fds[curr].prev = idx;
}

static void do_sock_read(my_ipc_cb_t *cb,int fd)
{
    int rc;

    do
    {
        rc = read(fd,SOCK_SERVER_CB(cb).buffer,cb->buf_size);
        if(rc > 0)
        {
            //printf("read %d\n",rc);
            cb->total_read += rc;
        }
    }while(EDGE_TRIGGER);
}

static void do_sock_write(my_ipc_cb_t *cb,int fd)
{
    int rc;
    sprintf(SOCK_SERVER_CB(cb).buffer,"JURA HOY%d",g_seq++);
    do
    {
        rc = write(fd,SOCK_SERVER_CB(cb).buffer,cb->buf_size);
        if(rc > 0)
        {
            //printf("written %d\n",rc);
            cb->total_written += rc;
        }
    }while(EDGE_TRIGGER);
}

static void do_sock_test_left_side(my_ipc_cb_t *cb)
{
    int i;

    struct epoll_event events[MAX_CLIENTS+1];   
    struct epoll_event new_event;
    int events_occured,new_sock,len;
    struct sockaddr sa;
 
    events[0].events = EPOLLIN | EPOLLOUT;
    events[0].data.fd = SOCK_FDESCR(cb); 
    epoll_ctl(SOCK_EPOLL_DESCR(cb),EPOLL_CTL_ADD,SOCK_FDESCR(cb),&events[0]);

    while(1)
    {       
       events_occured = epoll_wait(SOCK_EPOLL_DESCR(cb),events,MAX_CLIENTS+1,-1);
       for(i = 0;i < events_occured;i++)
       {
           if(events[i].data.fd == SOCK_FDESCR(cb))
           {
               len = sizeof(sa);
               new_sock = accept(SOCK_FDESCR(cb),&sa,&len);
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
               new_event.data.fd = SOCK_FDESCR(cb);
               epoll_ctl(SOCK_EPOLL_DESCR(cb),EPOLL_CTL_ADD,SOCK_FDESCR(cb),&new_event);
               if(new_sock > 0)
               {
                   do_sock_write(cb,new_sock);
		           new_event.events = EPOLLIN | EPOLLOUT | EDGE_TRIGGER;
                   new_event.data.fd = new_sock;
                   epoll_ctl(SOCK_EPOLL_DESCR(cb),EPOLL_CTL_ADD,new_sock,&new_event);
               }
           }
           else if(events[i].events & EPOLLIN)
           {
               do_sock_read(cb,events[i].data.fd);
	           //new_event.events = EPOLLIN | EPOLLOUT | EDGE_TRIGGER;
               //new_event.data.fd = events[i].data.fd;
               //epoll_ctl(SOCK_EPOLL_DESCR(cb),EPOLL_CTL_ADD,events[i].data.fd,&new_event);
           }
           else if(events[i].events & EPOLLOUT)
           {
               do_sock_write(cb,events[i].data.fd);
	           //new_event.events = EPOLLIN | EPOLLOUT | EDGE_TRIGGER;
               //new_event.data.fd = events[i].data.fd;
               //epoll_ctl(SOCK_EPOLL_DESCR(cb),EPOLL_CTL_ADD,events[i].data.fd,&new_event);
           }
           else
           {
               printf("%s %d\n",__FILE__,__LINE__);
           }
       }
       if((cb->total_written >= cb->termination_criteria)||(cb->total_read >= cb->termination_criteria)) 
       {
//            iterate_active_fds(cb,clean_up);
//            clean_up(cb,SOCK_FDESCR(cb));
//            break;
	      register_end_of_test(cb);
	      print_test_results(cb);
	      cb->total_written = 0;
	      cb->total_read = 0;
	      register_start_of_test(cb);
       }  
   }
}

static void do_sock_test(my_ipc_cb_t *cb)
{
    do_sock_test_left_side(cb);
}

static void print_test_results(my_ipc_cb_t *cb)
{
    unsigned int secs = cb->end_tv.tv_sec - cb->start_tv.tv_sec;
    unsigned int usecs = cb->end_tv.tv_usec - cb->start_tv.tv_usec;
    printf("r%u w%u bytes in %d seconds %d usec\n",cb->total_read,cb->total_written,secs,usecs);
    if(secs > 0)
    {
        printf("which is w%u r%u  bytes/sec\n",cb->total_written/secs,cb->total_read/secs);
    }
}

static void register_start_of_test(my_ipc_cb_t *cb)
{
    gettimeofday(&cb->start_tv,NULL);
}

static void register_end_of_test(my_ipc_cb_t *cb)
{
    gettimeofday(&cb->end_tv,NULL);
}

static void clean_up(my_ipc_cb_t *cb,int fd)
{
    if(SOCK_FDESCR(cb) != 0)
    {
        shutdown(SOCK_FDESCR(cb),SHUT_RDWR);
        close(SOCK_FDESCR(cb));
        SOCK_FDESCR(cb) = 0;
    }
    shutdown(fd,SHUT_RDWR);
    close(fd);
}

static void init_client_sockets(my_ipc_cb_t *cb)
{
    int i;

    for(i = 0;i < cb->client_connections_number;i++)
    {
	printf("init client socket %d\n",i);
        init_client_socket(cb);
    }
}

void do_test(void *arg)
{
    my_ipc_cb_t *cb = (my_ipc_cb_t *)arg;

   SOCK_SERVER_CB(cb).buffer = (char *)malloc(cb->buf_size);
   if(SOCK_SERVER_CB(cb).buffer == NULL)
   {
        printf("memory allocation failure %s %d\n",__FILE__,__LINE__);
        exit(0);
   }
   memset(SOCK_SERVER_CB(cb).buffer,0xEE,cb->buf_size);
   init_server_sock(cb);
   init_client_sockets(cb);
   register_start_of_test(cb);
   do_sock_test(cb);
   register_end_of_test(cb);
   print_test_results(cb);
   free(cb);
}

void *init_test(int buf_size,
                int instance_id,
                unsigned int termination_criteria,
                int client_connections_number,
                int client_side_port_base,
                int server_side_port_base,
		unsigned int client_ip,
		unsigned int server_ip)
{
    my_ipc_cb_t *cb;

    cb = (my_ipc_cb_t *)malloc(sizeof(my_ipc_cb_t));
    if(cb == NULL)
    {
        printf("cannot allocate CB %s %d\n",__FILE__,__LINE__);
        exit(0);
    }
    client_side_port_base = htons(client_side_port_base);
    server_side_port_base = htons(server_side_port_base);
    memset(cb,0,sizeof(*cb));
    cb->buf_size = buf_size;
    cb->instance = instance_id;
    cb->termination_criteria = termination_criteria;
    cb->client_connections_number = client_connections_number;
    cb->client_side_port_base = client_side_port_base;
    cb->server_side_port_base = server_side_port_base;
    cb->client_ip = client_ip;
    cb->server_ip = server_ip;
    init_free_fds(cb);
    return cb;
}

int main(int argc, char **argv)
{
    if(argc != 9)
    {
        printf("Usage:  <buf_size> <instance_id> <term crit> <client conn num> <client port base> <server port base> <connectip> <acceptip>\n");
        exit(1);
    }
    printf("Entered: buf_size %d instance_id %d term crit %d client conn num %d client port base %d server port base %d\n",
           atoi(argv[1]),atoi(argv[2]),atoi(argv[3]),atoi(argv[4]),atoi(argv[5]),atoi(argv[6]));
    do_test(init_test(atoi(argv[1]),atoi(argv[2]),atoi(argv[3]),atoi(argv[4]),atoi(argv[5]),atoi(argv[6]),inet_addr(argv[7]),inet_addr(argv[8])));
    return 0;
}
