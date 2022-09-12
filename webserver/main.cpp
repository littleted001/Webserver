#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"

#define MAX_FD 65536   // 最大的文件描述符个数
#define MAX_EVENT_NUMBER 10000  // 监听的最大的事件数量

// 添加文件描述符，
//在http_conn中实现，在此处把它声明成外部的，其他程序也可以调用，epoll one_shot事件（一个socket连接在任一时刻都只被一个线程处理）
extern void addfd( int epollfd, int fd, bool one_shot );
extern void removefd( int epollfd, int fd );

void addsig(int sig, void( handler )(int)){    //第二个参数是函数指针
    struct sigaction sa;  //sa是注册信号的参数
    memset( &sa, '\0', sizeof( sa ) );
    sa.sa_handler = handler;
    sigfillset( &sa.sa_mask );  //设置临时阻塞的信号集
    assert( sigaction( sig, &sa, NULL ) != -1 );
}

int main( int argc, char* argv[] ) {
    
    if( argc <= 1 ) {  //啥也不传只有一个参数（默认参数，是程序名，还需传一个端口号
        printf( "usage: %s port_number\n", basename(argv[0]));
        return 1;
    }
    //获取端口号，并把字符串转换为整型
    int port = atoi( argv[1] );
    //网络通信时一端断开，一端还往里面写，会产生SIGPIE信号
    //对SIGPIE信号进行处理，捕捉到后忽略它，不会结束程序
    addsig( SIGPIPE, SIG_IGN );

    //创建线程池，http_conn就是线程池需要处理的任务类
    threadpool< http_conn >* pool = NULL;
    try {
        pool = new threadpool<http_conn>;
    } catch( ... ) {
        return 1;
    }
    //创建一个数组用于保存所有的客户端信息
    http_conn* users = new http_conn[ MAX_FD ];

    int listenfd = socket( PF_INET, SOCK_STREAM, 0 );

    int ret = 0;
    struct sockaddr_in address;
    address.sin_addr.s_addr = INADDR_ANY;  //任何地址
    address.sin_family = AF_INET;   //TCP/IPv4协议族
    address.sin_port = htons( port );  //转换网络字节序，port通过main函数的参数已经获取

    // 设置端口复用，一定要在绑定前设置
    //端口复用最常见的用途：防止服务器重启之前绑定的端口还未释放；或程序突然退出而系统没有释放端口
    int reuse = 1;   //1表示复用
    setsockopt( listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) );

    ret = bind( listenfd, ( struct sockaddr* )&address, sizeof( address ) );
    ret = listen( listenfd, 5 );

    // 创建epoll对象，和事件数组，添加
    epoll_event events[ MAX_EVENT_NUMBER ];
    int epollfd = epoll_create( 5 );  ////创建epoll对象，参数可以传任何非0值
    //自定义函数，添加到epoll对象中
    addfd( epollfd, listenfd, false );
    http_conn::m_epollfd = epollfd;  //设置静态成员，所有任务共享一个epoll对象，在.h文件中定义

    while(true) {
        //检测到变化的事件数，epoll_wait默认是阻塞的，-1表示阻塞，收到信号不阻塞，往下执行  
        int number = epoll_wait( epollfd, events, MAX_EVENT_NUMBER, -1 );
        //返回值小于0且非中断的情况，epoll失败      
        if ( ( number < 0 ) && ( errno != EINTR ) ) {
            printf( "epoll failure\n" );
            break;
        }

        //循环遍历事件数组，number统计到在双链表中变化的文件描述符
        for ( int i = 0; i < number; i++ ) {
            //监听到的文件描述符，从事件数组中取出事件，进行处理
            int sockfd = events[i].data.fd;
            
            if( sockfd == listenfd ) {
                
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof( client_address );
                int connfd = accept( listenfd, ( struct sockaddr* )&client_address, &client_addrlength );
                
                if ( connfd < 0 ) {
                    printf( "errno is: %d\n", errno );
                    continue;
                } 

                if( http_conn::m_user_count >= MAX_FD ) {
                    //目前连接数满了
                   //给客户端写信息如服务器正忙（回复涉及到响应报文），直接回复信息客户端（浏览器）是解析不了的
                    close(connfd);
                    continue;
                }
                //将新的客户的数据初始化，放到数组中，用连接的文件描述符作为数组索引，对其初始化，若文件描述符没有关闭，则为递增
                //默认初始化为0
                users[connfd].init( connfd, client_address);
              
              //判断普通通信文件描述符
            } else if( events[i].events & ( EPOLLRDHUP | EPOLLHUP | EPOLLERR ) ) {
                //对方异常断开或错误等事件，关闭连接,(自定义函数，不是简单的close)
                users[sockfd].close_conn();

                //判断是否有读事件发生，有的话一次性读出来
            } else if(events[i].events & EPOLLIN) {
                //按照模拟proactor模式的做法一次性读出
                if(users[sockfd].read()) {  //一次性读，自定义读函数，非阻塞读， 系统提供的读函数是阻塞的，会饿死其他线程
                    pool->append(users + sockfd);
                } else {
                    users[sockfd].close_conn();
                }
                //检测写事件
            }  else if( events[i].events & EPOLLOUT ) {

                if( !users[sockfd].write() ) {    //一次性写完，非阻塞写，写失败则关闭
                    users[sockfd].close_conn();
                }

            }
        }
    }

    //connfd在处理的过程中已经及时关闭
    close( epollfd );
    close( listenfd );
    delete [] users;
    delete pool;
    return 0;
}