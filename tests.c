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
#include <netinet/tcp.h>
#include <sys/ioctl.h> 
#include <pthread.h>
#include <signal.h>
#include <sys/queue.h>
#include <time.h>



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

struct socket_entry
{
	int fd;
	int is_in_read_q;
	int is_in_write_q;
	TAILQ_ENTRY(socket_entry) read_q_entry;
	TAILQ_ENTRY(socket_entry) write_q_entry;
};

typedef struct
{
    int client_side_port_base;
    int server_side_port_base;
    unsigned int client_ip;
    unsigned int server_ip;
    int buf_size;
    unsigned char *buffer; 
    int client_connections_number;
    int writer_epoll_fd; 
    int fds[MAX_CLIENTS];
    int epoll_fd;
    struct socket_entry listener_socket_entry;
    int client_idx;
    int type;
    int rxtx_flag;
    unsigned char *rr_buffer;
    int rr_buf_size;
    int read_q_entries_count;
    int write_q_entries_count;
    TAILQ_HEAD(, read_q) read_q;
    TAILQ_HEAD(, write_q) write_q;
}cb_t;

struct timeval start_tv;
int termination_criteria;
uint64_t total_written = 0;
uint64_t total_read = 0;
uint64_t rr_average = 0;
uint64_t rr_count = 0;
struct timeval g_tv;

#define RX_ON 0x1
#define TX_ON 0x2
#define LATENCY_CLIENT 0x4
#define LATENCY_SERVER 0x8
#define MODE_TCP 1
#define MODE_UDP 2

int g_trigger = EDGE_TRIGGER; 

cb_t *cbs = NULL;
pthread_t *threads;

static void clean_up(int fd);

static void remove_from_read_queue(cb_t *cb, struct socket_entry *p_socket_entry)
{
	p_socket_entry->is_in_read_q = 0;
	cb->read_q_entries_count--;
	TAILQ_REMOVE(&cb->read_q, p_socket_entry, read_q_entry);
}

static void remove_from_write_queue(cb_t *cb, struct socket_entry *p_socket_entry)
{
	p_socket_entry->is_in_write_q = 0;
	cb->write_q_entries_count--;
	TAILQ_REMOVE(&cb->write_q, p_socket_entry, write_q_entry);
}

static void insert_into_read_queue(cb_t *cb, struct socket_entry *p_socket_entry)
{
	p_socket_entry->is_in_read_q = 1;
	cb->read_q_entries_count++;
	TAILQ_INSERT_TAIL(&cb->read_q, p_socket_entry, read_q_entry);
}

static void insert_into_write_queue(cb_t *cb, struct socket_entry *p_socket_entry)
{
	p_socket_entry->is_in_write_q = 1;
	cb->write_q_entries_count++;
	TAILQ_INSERT_TAIL(&cb->write_q, p_socket_entry, write_q_entry);
}

static void init_server_sock(cb_t *cb)
{
    struct sockaddr *sa;
    int len;
    int val; 
    struct sockaddr_in sockaddrin;

    memset(&cb->listener_socket_entry,0,sizeof(struct socket_entry));

    cb->epoll_fd = epoll_create(10000);

    if(cb->type != MODE_TCP) {
        return;
    }

    sockaddrin.sin_family = AF_INET;
    sockaddrin.sin_port = htons(cb->client_side_port_base);
    sockaddrin.sin_addr.s_addr = cb->server_ip;
    sa = (struct sockaddr *)&sockaddrin;
    len = sizeof(sockaddrin);
    
    cb->listener_socket_entry.fd = socket(AF_INET,SOCK_STREAM,0);
    if(cb->listener_socket_entry.fd <= 0) {
        printf("PANIC: cannot open socket %s %d %d\n",__FILE__,__LINE__,errno);
        exit(2);
    }
    val = 1;
    setsockopt(cb->listener_socket_entry.fd,SOL_SOCKET, SO_REUSEADDR, &val,sizeof(val));
    val = 1;
    if(ioctl(cb->listener_socket_entry.fd,FIONBIO,(char *)&val)) {
        printf("cannot go non-blocking mode\n");
    } 
    if(bind(cb->listener_socket_entry.fd,sa,len) < 0) {
        printf("PANIC: cannot bind %s %d %d\n",__FILE__,__LINE__,errno);
        exit(3);
    }
    if(cb->rxtx_flag & LATENCY_SERVER) {
         val = 1;
         setsockopt(cb->listener_socket_entry.fd,IPPROTO_TCP, TCP_NODELAY, &val,sizeof(val));
    }
    if(listen(cb->listener_socket_entry.fd,10000) < 0) {
        printf("PANIC: cannot listen %s %d %d\n",__FILE__,__LINE__,errno);
    }
    printf("listener created %d\n",ntohs(sockaddrin.sin_port));
}

static void init_client_socket(cb_t *cb)
{
    struct sockaddr *sa;
    int len;
    int val;
    struct epoll_event new_event;
    struct sockaddr_in sockaddrin;
    int family = (cb->type == 1) ? SOCK_STREAM : SOCK_DGRAM;
    struct socket_entry *p_socket_entry = malloc(sizeof(struct socket_entry));
    sockaddrin.sin_family = AF_INET;
    sockaddrin.sin_port = htons(cb->server_side_port_base);
    sockaddrin.sin_addr.s_addr = cb->client_ip;
    sa = (struct sockaddr *)&sockaddrin;
    len = sizeof(sockaddrin);
    if (!p_socket_entry) {
	printf("cannot allocate socket entry %s %d\n",__FILE__,__LINE__);
	exit(0);
    }
    memset(p_socket_entry,0,sizeof(struct socket_entry));

    p_socket_entry->fd = socket(AF_INET,family,0);
    if(p_socket_entry->fd <= 0) {
        printf("PANIC: cannot open socket %s %d %d\n",__FILE__,__LINE__,errno);
        exit(2);
    }
    val = 1;
    setsockopt(p_socket_entry->fd,SOL_SOCKET, SO_REUSEADDR, &val,sizeof(val));
    val = 1;
    if(ioctl(p_socket_entry->fd,FIONBIO,(char *)&val)) {
        printf("cannot go non-blocking mode\n");
    }
    new_event.events = 0;
    if(cb->type != MODE_TCP) { 
        if(cb->rxtx_flag & RX_ON)
            new_event.events |= EPOLLIN;
         if(cb->rxtx_flag & TX_ON)
            new_event.events |= EPOLLOUT;
         if(bind(p_socket_entry->fd,sa,len) < 0) {
            printf("PANIC: cannot bind %s %d %d\n",__FILE__,__LINE__,errno);
            exit(3);
         }
    }
    else {
        new_event.events |= EPOLLIN;
        if(cb->rxtx_flag & (TX_ON|LATENCY_CLIENT))
            new_event.events |= EPOLLOUT;
    }
    new_event.data.ptr =  p_socket_entry;
    epoll_ctl(cb->epoll_fd,EPOLL_CTL_ADD,p_socket_entry->fd,&new_event);
    if((cb->type == MODE_TCP)&&(connect(p_socket_entry->fd,sa,len) < 0)) {
    }
    cb->fds[cb->client_idx++] = p_socket_entry->fd;
    if(cb->rxtx_flag & LATENCY_CLIENT) {
         val = 1;
         setsockopt(p_socket_entry->fd,IPPROTO_TCP, TCP_NODELAY, &val,sizeof(val));
    }
}

static void do_tcp_sock_read(cb_t *cb, struct socket_entry* p_socket_entry)
{
    int rc;
    unsigned int secs;
    unsigned int usecs;
    struct timeval tv,tv1;
    unsigned int val1,val2;
    struct epoll_event new_event;
    int read_this_time = 0;

    do {
        rc = read(p_socket_entry->fd,cb->buffer,cb->buf_size);
        if(rc > 0) {
	    read_this_time += rc;
            __sync_fetch_and_add(&total_read,rc);
            if(cb->rxtx_flag & LATENCY_SERVER) { 
                if((cb->buffer[0] == 0x7F)&&
                   (cb->buffer[1] == 0xF7)&&
                   (cb->buffer[2] == 0xA5)&&
                   (cb->buffer[3] == 0x5A)&&
                   (cb->buffer[4+sizeof(struct timeval)] == 0x7F)&&
                   (cb->buffer[5+sizeof(struct timeval)] == 0xF7)&&
                   (cb->buffer[6+sizeof(struct timeval)] == 0xA5)&&
                   (cb->buffer[7+sizeof(struct timeval)] == 0x5A)) {
                    new_event.events = g_trigger;
                    new_event.events |= EPOLLOUT;
                    new_event.data.ptr = p_socket_entry;
                    epoll_ctl(cb->epoll_fd,EPOLL_CTL_MOD,p_socket_entry->fd,&new_event);
                    if(cb->rr_buffer) {
                        if(rc <= cb->rr_buf_size) {
                            memcpy(cb->rr_buffer,cb->buffer,rc);
                            cb->rr_buf_size = rc;
                        }
                        else {
                            free(cb->rr_buffer);
                            cb->rr_buffer = NULL;
                            cb->rr_buf_size = 0;
                        }
                    }
                    if(!cb->rr_buffer) {
                        cb->rr_buffer = malloc(rc);
                        cb->rr_buf_size = rc;
                        memcpy(cb->rr_buffer,cb->buffer,rc);
                    } 
                }
                else {
                    new_event.events = g_trigger;
                    new_event.events |= EPOLLIN;
                    new_event.data.ptr = p_socket_entry;
                    epoll_ctl(cb->epoll_fd,EPOLL_CTL_MOD,p_socket_entry->fd,&new_event);
                }
            }
            else if(cb->rxtx_flag & LATENCY_CLIENT) { 
                if((cb->buffer[0] == 0x7F)&&
                   (cb->buffer[1] == 0xF7)&&
                   (cb->buffer[2] == 0xA5)&&
                   (cb->buffer[3] == 0x5A)&&
                   (cb->buffer[4+sizeof(tv1)] == 0x7F)&&
                   (cb->buffer[5+sizeof(tv1)] == 0xF7)&&
                   (cb->buffer[6+sizeof(tv1)] == 0xA5)&&
                   (cb->buffer[7+sizeof(tv1)] == 0x5A)) {
                    new_event.events = g_trigger;
                    new_event.events |= EPOLLOUT;
                    new_event.data.ptr = p_socket_entry;
                    epoll_ctl(cb->epoll_fd,EPOLL_CTL_MOD,p_socket_entry->fd,&new_event);
                    bcopy(&cb->buffer[4],&tv1,sizeof(tv1));
                    gettimeofday(&tv,NULL);
                    val1 = tv1.tv_sec*1000000 + tv1.tv_usec;
                    val2 = tv.tv_sec*1000000 + tv.tv_usec;
                    if(val1 > val2){
                        printf("val1>val2!!! %u %u\n",val1,val2);
                    }
                    else {
                        rr_average += val2 - val1;
                        rr_count++;
                        //printf("rtt %u sec %u %u usec %u %u\n",val2-val1,tv1.tv_sec,tv.tv_sec,tv1.tv_usec,tv.tv_usec);
                    }
                }
                else {
                    new_event.events = g_trigger;
                    new_event.events |= EPOLLOUT|EPOLLIN;
                    new_event.data.ptr = p_socket_entry;
                    epoll_ctl(cb->epoll_fd,EPOLL_CTL_MOD,p_socket_entry->fd,&new_event);
                }
            }
	    else if(read_this_time > (cb->buf_size << 8)) {
		if (!p_socket_entry->is_in_read_q) {
			insert_into_read_queue(cb, p_socket_entry);
		}
		break;
	    }
        }
        else {
	    if (p_socket_entry->is_in_read_q) {
		remove_from_read_queue(cb,p_socket_entry);
	    }
            break;
        }
    }while(g_trigger);
}

static void do_udp_sock_read(cb_t *cb,struct socket_entry* p_socket_entry)
{
    int rc,addr_len,read_this_time = 0;
    struct sockaddr *sa;
    struct sockaddr_in sockaddrin;
    sa = (struct sockaddr *)&sockaddrin;
    addr_len = sizeof(sockaddrin);
    do {
    	rc = recvfrom(p_socket_entry->fd,cb->buffer,cb->buf_size,0,sa,&addr_len);
        if(rc > 0) {
            __sync_fetch_and_add(&total_read,rc);
	    read_this_time += rc;
	    if(read_this_time > (cb->buf_size << 2)) {
	        if (!p_socket_entry->is_in_read_q) {
			insert_into_read_queue(cb, p_socket_entry);
		}
		break;
	    }
        }
        else {
            break;
        }
    }while(g_trigger);
}

static void do_tcp_sock_write(cb_t *cb, struct socket_entry* p_socket_entry)
{
    int rc,size,written_this_time = 0;
    struct timeval tv;
    struct epoll_event new_event;

    if(cb->rxtx_flag & LATENCY_CLIENT) {
        cb->buffer[0] = 0x7F;
        cb->buffer[1] = 0xF7;
        cb->buffer[2] = 0xA5;
        cb->buffer[3] = 0x5A;
        gettimeofday(&tv,NULL);
        bcopy(&tv,&cb->buffer[4],sizeof(tv)); 
        cb->buffer[4+sizeof(tv)] = 0x7F;
        cb->buffer[5+sizeof(tv)] = 0xF7;
        cb->buffer[6+sizeof(tv)] = 0xA5;
        cb->buffer[7+sizeof(tv)] = 0x5A; 
        size = 8+sizeof(tv);
        new_event.events = g_trigger;
        new_event.events |= EPOLLIN;
        new_event.data.ptr = p_socket_entry;
        epoll_ctl(cb->epoll_fd,EPOLL_CTL_MOD,p_socket_entry->fd,&new_event);
    }
    else if(cb->rxtx_flag & LATENCY_SERVER) {
        if(cb->rr_buffer) {
            memcpy(cb->buffer,cb->rr_buffer,cb->rr_buf_size);
            size = cb->rr_buf_size;
            free(cb->rr_buffer);
            cb->rr_buffer = NULL;
            cb->rr_buf_size = 0;
            new_event.events = g_trigger;
            new_event.events |= EPOLLIN;
            new_event.data.ptr = p_socket_entry;
            epoll_ctl(cb->epoll_fd,EPOLL_CTL_MOD,p_socket_entry->fd,&new_event);
        }
        else {
            memset(cb->buffer,0,cb->buf_size);
            size = cb->buf_size;
            new_event.events = g_trigger;
            new_event.events |= EPOLLIN;
            new_event.data.ptr = p_socket_entry;
            epoll_ctl(cb->epoll_fd,EPOLL_CTL_MOD,p_socket_entry->fd,&new_event);
        }
    }
    else {
    //    sprintf(cb->buffer,"JURA HOY%d",g_seq++);
        memset(cb->buffer,'H',cb->buf_size);
        size = cb->buf_size;
    }
    do {
        rc = write(p_socket_entry->fd,cb->buffer,size);
        if(rc > 0) {
            __sync_fetch_and_add(&total_written,rc);
	   written_this_time += rc;
	    if(written_this_time > (cb->buf_size << 8)) {
		if (!p_socket_entry->is_in_write_q) {
			insert_into_write_queue(cb, p_socket_entry);
		}
		break;
	    }
        }
        else {
	    if (p_socket_entry->is_in_write_q) {
		remove_from_write_queue(cb,p_socket_entry);
	    }
            break;
        }
    }while(g_trigger);
}

static void do_udp_sock_write(cb_t *cb,struct socket_entry* p_socket_entry)
{
    int rc,written_this_time = 0;
    struct sockaddr *sa;
    struct sockaddr_in sockaddrin;
    sprintf(cb->buffer,"JURA HOY%d",g_seq++);
    sockaddrin.sin_family = AF_INET;
    sockaddrin.sin_port = htons(cb->server_side_port_base);
    sockaddrin.sin_addr.s_addr = cb->server_ip;
    sa = (struct sockaddr *)&sockaddrin;
    do
    {
    	rc = sendto(p_socket_entry->fd,cb->buffer,cb->buf_size,0,sa,sizeof(sockaddrin));
        if(rc > 0) {
	    written_this_time += rc;
            __sync_fetch_and_add(&total_written,rc);
	    if(written_this_time > (cb->buf_size << 2)) {
		if (!p_socket_entry->is_in_write_q) {
			insert_into_write_queue(cb, p_socket_entry);
		}
		break;
	    }
        }
        else {
            break;
        }
    }while(g_trigger);
}

static void do_sock_test_left_side(cb_t *cb)
{
    int i,iterations = 0;

    struct epoll_event events[MAX_CLIENTS+1];   
    struct epoll_event new_event;
    int events_occured,new_sock,len,entries_to_process;
    struct sockaddr sa;
    struct socket_entry *p_socket_entry;
 
    if(cb->type == MODE_TCP) {
        events[0].events = EPOLLIN;
        events[0].data.ptr = &cb->listener_socket_entry;
        epoll_ctl(cb->epoll_fd,EPOLL_CTL_ADD,cb->listener_socket_entry.fd,&events[0]);
    }

    while(1) {  
       p_socket_entry = TAILQ_FIRST(&cb->read_q);
       entries_to_process = cb->read_q_entries_count;
       while (p_socket_entry && (entries_to_process > 0)) {
		struct socket_entry *p_next = TAILQ_NEXT(p_socket_entry, read_q_entry);
		remove_from_read_queue(cb,p_socket_entry);
		do_tcp_sock_read(cb, p_socket_entry);
		p_socket_entry = p_next;
		entries_to_process--;
       }
       p_socket_entry = TAILQ_FIRST(&cb->write_q);
       entries_to_process = cb->write_q_entries_count;
       while (p_socket_entry && (entries_to_process > 0)) {
		struct socket_entry *p_next = TAILQ_NEXT(p_socket_entry, write_q_entry);
		remove_from_write_queue(cb,p_socket_entry);
		do_tcp_sock_write(cb, p_socket_entry);
		p_socket_entry = p_next;
		entries_to_process--;
       }
       events_occured = epoll_wait(cb->epoll_fd,events,MAX_CLIENTS+1, (cb->read_q_entries_count || cb->write_q_entries_count) ? 0 : -1);
       for(i = 0;i < events_occured;i++) {
           if(events[i].data.ptr == &cb->listener_socket_entry) {
               len = sizeof(sa);
               new_sock = accept(cb->listener_socket_entry.fd,&sa,&len);
               if(new_sock <= 0) {
                   printf("a problem in accept %d\n",errno);
                   /*exit(5);*/
               }
               else {
                   p_socket_entry = malloc(sizeof(struct socket_entry));
		   if(!p_socket_entry) {
			printf("cannot allocate memory %s %d\n",__FILE__,__LINE__);
			exit(0);
		   }
		   memset(p_socket_entry,0,sizeof(struct socket_entry));
		   p_socket_entry->fd = new_sock;
               }
               new_event.events = EPOLLIN | EPOLLOUT;
               new_event.data.ptr = &cb->listener_socket_entry;
               epoll_ctl(cb->epoll_fd,EPOLL_CTL_ADD,cb->listener_socket_entry.fd,&new_event);
               if(new_sock > 0) {
                   new_event.events = g_trigger;
                   if(cb->rxtx_flag & (LATENCY_CLIENT|LATENCY_SERVER)) {
                       if(cb->rxtx_flag & LATENCY_CLIENT) {
                           do_tcp_sock_write(cb,events[i].data.ptr);
                           new_event.events |= EPOLLOUT;
                       }
                       new_event.events |= EPOLLIN;
                   }
                   else {
                       if(cb->rxtx_flag & TX_ON) {
                           do_tcp_sock_write(cb,events[i].data.ptr);
                           new_event.events |= EPOLLOUT;
                       }
                       if(cb->rxtx_flag & RX_ON) {
  	                   new_event.events |= EPOLLIN;
                       } 
                   }
                   new_event.data.ptr = p_socket_entry;
                   epoll_ctl(cb->epoll_fd,EPOLL_CTL_ADD,new_sock,&new_event);
               }
           }
           if((&cb->listener_socket_entry != events[i].data.ptr)&&(events[i].events & EPOLLIN)&&(cb->rxtx_flag & (RX_ON|LATENCY_CLIENT|LATENCY_SERVER))) {
               if(cb->type == MODE_TCP)
                   do_tcp_sock_read(cb,events[i].data.ptr);
               else
                   do_udp_sock_read(cb,events[i].data.ptr);
           }
           if((&cb->listener_socket_entry != events[i].data.ptr)&&(events[i].events & EPOLLOUT)&&(cb->rxtx_flag & (TX_ON|LATENCY_CLIENT|LATENCY_SERVER))) {
               if(cb->type == MODE_TCP)
                   do_tcp_sock_write(cb,events[i].data.ptr);
               else
                   do_udp_sock_write(cb,events[i].data.ptr);
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

    if(rxtx_flag & (LATENCY_CLIENT|LATENCY_SERVER)) {
        if(number_of_threads > 1) {
            printf("If latency is measured, only one thread must be configured\n");
            exit(0);
        }
        gettimeofday(&g_tv,NULL);
    }

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
	cbs[idx].write_q_entries_count = 0;
	cbs[idx].read_q_entries_count = 0;
	TAILQ_INIT(&cbs[idx].read_q);
    	TAILQ_INIT(&cbs[idx].write_q);
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
    int duration = 1500000;
    int i;

    if(argc < 9) {
        printf("Usage:  <buf_size> <number_of_threads> <bytes rx/tx to print stats> <client conn num> <client port base> <server port base> <connectip> <acceptip> <type (1-tcp,2-udp> [rxtx (0x1 - write, 0x2 - read)]\n");
        exit(1);
    }
    if(argc == 11) {
        rxtx = atoi(argv[10]);
    }
    if(rxtx & (LATENCY_CLIENT|LATENCY_SERVER)) {
        g_trigger = 0;
        duration = 200;
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
    if(rxtx & LATENCY_CLIENT) {
        if(rr_count == 0)
            printf("0 responses received\n");
        else
            printf("latency(avg) %f micro\n",(float)rr_average / (float)rr_count);
    }
    else
        system("cat /proc/interrupts");
    return 0;
}
