#include <event2/event.h>
#include "svr_config.h"
#include "svr_log.h"
#include "iohelper.h"
#include "common.h"
#include "dlock.h"
#include <unistd.h>
#include <getopt.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include "dlop.h"

int is_stop = false;
struct frame_config svrconf;
dlock_t thread_lock;
struct frame_callback cb;
struct net_util ncb;

void thread_client(evutil_socket_t sock, short ev_flag, void* arg);
/**
 * if modified, reset event, and readd to event_base
 */
#define connection_set_return_on_error(conn, fd, origflag)    do{                 \
        if((conn)->evt_mask != (origflag)){                                             \
            if(event_assign((conn)->evt, (conn)->ep->base, (fd), (conn)->evt_mask,      \
                thread_client, (conn)) == 0){}                                          \
            else{                                                                       \
                error("reset event listen failed");                                     \
                return -1;                                                              \
            }                                                                           \
        }                                                                               \
        if(event_add((conn)->evt, &(conn)->timeout) == 0){}                             \
        else{                                                                           \
            error("add event failed");                                                  \
            return -1;                                                                  \
        }                                                                               \
    }while(0)

int init_svr_config(const char* file)
{
    if(init_config(&svrconf) != 0){
        return -1;
    }
    if(load_config(file, &svrconf) != 0){
        return -1;
    }
    return 0;
}

int init_callback()
{
    struct stat fst;
    if(svrconf.cblib == NULL || svrconf.networklib == NULL){
        error("dll path not set!");
        return -1;
    }
    if(stat(svrconf.cblib, &fst) != 0){
        error("dll file not exist!");
        return -1;
    }

    if(load_callback(svrconf.cblib, &cb) == -1){
        error("load callback failed !");
        return -1;
    }
    
    if(stat(svrconf.networklib, &fst) != 0){
        error("network dll not exist!");
        return -1;
    }
    if(load_net_util(svrconf.networklib, &ncb) == -1){
        error("load network callback failed!");
        return -1;
    }
    return 0;
}

void finish_callback()
{
    fini_callback(&cb);
    fini_net_util(&ncb);
}

int create_listen_sock()
{
    int reuse_addr = 1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd == -1){
        error("create socket failed: %s", strerror(errno));
        goto fail;
    }
    if(set_nonblock(fd) != 0){ //set nonblock
        error("set nonblock failed:%s", strerror(errno));
        goto fail;
    }
    if(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse_addr, sizeof(reuse_addr)) != 0){
        //set reuse addr
        error("set reuseaddr failed:%s", strerror(errno));
        goto fail;
    }
    if(setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &svrconf.nodelay, sizeof(svrconf.nodelay)) != 0){
        //disable nagle algorithm
        error("set nodelay failed:%s", strerror(errno));
        goto fail;
    }
    if(setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &svrconf.send_buffer_size, sizeof(svrconf.send_buffer_size)) != 0){
        //set send buffer size
        error("set send_buffer_size failed:%s", strerror(errno));
        goto fail;
    }
    if(setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &svrconf.recv_buffer_size, sizeof(svrconf.recv_buffer_size)) != 0){
        //set recv buffer size
        error("set recv_buffer_size failed:%s", strerror(errno));
        goto fail;
    }
    struct sockaddr_in svraddr;
    svraddr.sin_family = AF_INET;
    svraddr.sin_port = htons(svrconf.listen_port);
    svraddr.sin_addr.s_addr = inet_addr(svrconf.listen_ip);
    if(bind(fd, (struct sockaddr*)&svraddr, sizeof(svraddr)) != 0){ //bind to addr
        error("bind addr[%s:%d] failed:%s", svrconf.listen_ip, svrconf.listen_port, strerror(errno));
        goto fail;
    }
    if(listen(fd, svrconf.backlog) != 0){ //begin listen
        error("listen failed:%s, backlog:%d", strerror(errno), svrconf.backlog);
        goto fail;
    }
    return fd;
fail:
    if(fd != -1){
        close(fd);
        fd = 0;
    }
    return -1;
}
void handle_sigint(int sig)
{
    is_stop = true;
}

void handle_child(int sig)
{
    pid_t child;
    int status;
    while((child = waitpid(-1, &status, WNOHANG)) > 0);
    signal(SIGCHLD, handle_child);
}

int setup_signal()
{
    //TODO setup signal functions
    if(SIG_ERR == signal(SIGINT, handle_sigint)){
        error("set SIGINT handler failed:%s", strerror(errno));
        return -1;
    }
    if(SIG_ERR == signal(SIGPIPE, SIG_IGN)){
        error("set SIGPIPE handler failed:%s", strerror(errno));
        return -1;
    }
    
    if(SIG_ERR == signal(SIGCHLD, handle_child)){
        error("set SIGCHLD handler failed:%s", strerror(errno));
        return -1;
    }
    return 0;
}

void thread_timer(evutil_socket_t sock, short ev_flag, void* arg)
{
    //info("thread working!");
    //TODO add timing here
}

/**
 * recv package from client, then check if the package complete
 * if complete, process the package
 */
int handle_read(int sock, struct connection* conn)
{
    int ret = recv(sock, buffer_position(&(conn->readbuf)), buffer_space(&(conn->readbuf)), 0);
    switch(ret){
        case 0://connection closed
            info("connection closed by peer");
            return -1;
        case -1://error
            if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR){
                return 0;
            }else{
                error("recv failed:%s", strerror(errno));
                return -1;
            }
        default:
            buffer_write(&(conn->readbuf), ret);
    }
    //test if package complete, if ok, process the package
    int packlen = ncb.package_complete(conn->readbuf.buffer, conn->readbuf.size);
    if(packlen > 0){
        ret = cb.read_cb(sock, conn);
    }
    return ret;
}

/**
 * send package to client
 */
int handle_write(int sock, struct connection* conn)
{
    int ret = send(sock, buffer_position(&(conn->writebuf)), buffer_space(&(conn->writebuf)), 0);
    switch(ret){
        case 0://connection closed
            info("connection closed by peer in write");
            return -1;
        case -1:
            if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR){
                return 0;
            }else{
                error("send failed:%s", strerror(errno));
                return -1;
            }
        default:
            buffer_write(&(conn->writebuf), ret);
    }
    //test if package complete, if ok, process the package
    ret = cb.write_cb(sock, conn);
    return ret;
}

/**
 * init the connection, add user defined variable here
 */
int handle_init(int sock, struct connection* conn)
{
    return cb.init_cb(sock, conn);
}

/**
 * process on EV_TIMEOUT occurs
 */
int handle_timeout(int sock, struct connection* conn)
{
    return cb.timeout_cb(sock, conn);
}

void thread_client(evutil_socket_t sock, short ev_flag, void* arg)
{
    info("in client, flag:%d", ev_flag);
    int handle_result = 0;
    struct connection* conn = (struct connection*)arg;
    int evt_mask = conn->evt_mask;
    switch(ev_flag){
        case EV_READ:
            handle_result = handle_read(sock, conn);
            break;
        case EV_WRITE:
            handle_result = handle_write(sock, conn);
            break;
        case EV_TIMEOUT:
            handle_result = handle_timeout(sock, conn);
            break;
        default:
            handle_result = -1;
            error("unknown ev_flag occurs:%d", ev_flag);
    }
    switch(handle_result){
        case -1://failed
            goto err;
            break;
        case 0://OK, then continue next
        case 1://not complete
        default://unknown
            if(evt_mask != conn->evt_mask){
                if(event_assign(conn->evt, conn->ep->base, sock, conn->evt_mask, thread_client, conn) == 0){}
                else{
                    error("re_assign event failed, mask:%d", conn->evt_mask);
                    goto err;
                }
            }
            if(event_add(conn->evt, &conn->timeout) == 0){}
            else{
                error("add event to base failed");
                goto err;
            }
    }
    return;
err:
    cb.fini_cb(sock, conn);
    event_free(conn->evt);
    conn->evt = NULL;
    free_connection(conn);
    close(sock);
    return;
}

/**
 * accept client connection, setup event callback & add to event_base
 */
void thread_accept(evutil_socket_t sock, short ev_flag, void* arg)
{
    struct sockaddr_in addr;
    struct event_pool* ep = (struct event_pool*)arg;
    socklen_t addrlen = sizeof(addr);
    int fd = accept(sock, (struct sockaddr*)&addr, &addrlen);
    if(fd == -1){
        error("accept client faile:%s", strerror(errno));
        return;
    }
    struct connection* conn = get_connection(ep);
    if(!conn){
        error("not enough memory for handle this connection");
        close(fd);
        return;
    }
    conn->evt_mask = EV_READ;
    struct event* evt_client = event_new(ep->base, fd, conn->evt_mask, thread_client, conn);
    conn->evt = evt_client;
    int evt_mask = conn->evt_mask;
    if(handle_init(fd, conn) == 0){ //in handle_init, user may change the evt_mask
        if(evt_mask != conn->evt_mask){
            if(event_assign(conn->evt, ep->base, fd, conn->evt_mask, thread_client, conn) == 0){}
            else{
                error("assign event failed, mask:%d", conn->evt_mask);
                goto err;
            }
        }
        event_base_loopbreak(ep->base);
        if(event_add(conn->evt, &conn->timeout) == 0){}
        else{
            error("add event to base failed");
            goto err;
        }
    }else{
        error("init connection from client failed");
        goto err;
    }
    return;
err:
    event_free(conn->evt);
    conn->evt = NULL;
    free_connection(conn);
    close(fd);
}

/**
 * thread main function, in the main loop:
 * 1. trylock the listen fd, if get the lock, become the leader
 * 2. run event_loop()
 */
void* thread_loop(void* arg)
{
    int svrsock = (int) arg;
    struct event* evt_timer = NULL;
    struct event* evt_listen = NULL;
    int ret = -1;
    struct event_pool ep;
    if(fixed_pool_init(&ep.mempool, 1000, 1000) != 0){
        error("init memory pool failed!");
        goto err;
    }
    ep.is_leader = false;
    ep.cfg = event_config_new();
    if(!ep.cfg){
        error("init event config failed!");
        goto err;
    }else{
        event_config_require_features(ep.cfg, 0);
    }
    ep.base = event_base_new_with_config(ep.cfg);
    if(!ep.base){
        error("init event base failed!");
        goto err;
    }else{
        int feature = event_base_get_features(ep.base);
        if(feature & EV_FEATURE_ET) info("edge triger used");
        if(feature & EV_FEATURE_O1) info("feature 01 used");
        if(feature & EV_FEATURE_FDS) info("feature fds used");
    }
    evt_timer = event_new(ep.base, -1, EV_PERSIST, thread_timer, NULL);
    evt_listen = event_new(ep.base, svrsock, EV_READ | EV_PERSIST, thread_accept, &ep);
    if((!evt_timer) || (!evt_listen)){
        error("init svr sock or timer failed!");
        goto err;
    }
    
    struct timeval interval = {1, 0};
    event_add(evt_timer, &interval);
    while(!is_stop){
        info("loop again!");
        if(dlock_trylock(&thread_lock) == 0){
            event_add(evt_listen, NULL);
            ep.is_leader = true;
        }else{
            ep.is_leader = false;
        }
        int ret = event_base_loop(ep.base, 0);
        switch(ret){
            case -1:
                error("event base dispatch loop failed!");
                break;
            case 1:
                info("no events!");
                break;
            default:
                break;
        }
        if(ep.is_leader){
            event_del(evt_listen);
            dlock_unlock(&thread_lock);
            ep.is_leader = false;
        }
    }
    ret = 0;
err:
    event_free(evt_timer);
    event_free(evt_listen);
    evt_listen = NULL;
    evt_timer = NULL;
    event_pool_fini(&ep);
    return (void*) ret;
}

int main(int argc, char** argv)
{
    int is_daemon = 0;
    char conf_file[DPATH_MAX] = {0};
    char ch;
    pthread_t* workers = NULL;
    while((ch = getopt(argc, argv, "c:d")) != -1){
        switch(ch){
            case 'c':
                strncpy(conf_file, optarg, sizeof(conf_file));
                break;
            case 'd':
                is_daemon = 1;
                break;
            default:
                fprintf(stderr, "option %c is not defined\n", ch);
        }
    }
    (void)(sizeof(is_daemon));
    if(strlen(conf_file) == 0){
        fprintf(stderr, "not set config file\n");
        return -1;
    }
    if(init_svr_config(conf_file) != 0){
        fprintf(stderr, "init config failed\n");
        return -1;
    }
    if(init_log() != 0){
        fprintf(stderr, "init log failed\n");
        return -1;
    }
    if(setup_signal() != 0){
        return -1;
    }
    if(init_callback() != 0){
        return -1;
    }
    printf("config port:%d, config.ip:%s\n", svrconf.listen_port, svrconf.listen_ip);
    int sock = create_listen_sock();
    if(sock == -1){
        fprintf(stderr, "init socket failed\n");
        return -1;
    }
    info("socket listened, fd:%d!", sock);
    if(dlock_init(&thread_lock) != 0){
        error("init lock failed:%s", strerror(errno));
        return -1;
    }
    info("init threads, total:%d thread", svrconf.threads);
    if(svrconf.threads > 0){
        workers = (pthread_t*) malloc(sizeof(pthread_t) * svrconf.threads);
        for(int i = 0; i < svrconf.threads; ++i){
            if(pthread_create(workers + i, NULL, thread_loop, (void*)sock) != 0){
                error("dispatch thread failed:%s", strerror(errno));
                is_stop = true;
                return -1;
            }
        }
    }else{
        //create only one thread
        workers = (pthread_t*) malloc(sizeof(pthread_t));
        if(pthread_create(workers, NULL, thread_loop, (void*)sock) != 0){
            error("dispatch thread failed:%s", strerror(errno));
            is_stop = true;
            return -1;
        }
    }
    info("thread init ok!");
    int thread_count = svrconf.threads > 0 ? svrconf.threads : 1;
    //waiting all threads to finish
    for(int i = 0; i < thread_count; ++i){
        void *tresult;
        if(pthread_join(workers[i], &tresult) != 0){
            error("join thread:%d failed:%s", (pthread_t)workers[i], strerror(errno));
        }
    }
    free(workers);
    workers = NULL;
    info("all thread exit safely!");
    finish_callback();
    fini_log();
    fini_config(&svrconf);
    return 0;
}
