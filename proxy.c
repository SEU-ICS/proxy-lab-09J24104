// 09J24104 周启凡, 参考 https://arthals.ink/blog/proxy-lab
// 一些自我提醒, 大写字母开头的函数包装了 exit(0), 不管 errno 类型, 比较危险
// eg. Accept/accept, Close/close, Rio_/rio_
#include <stdio.h>
#include "csapp.h"
/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
#define MAX_CACHE_NUM 10

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";

typedef char string[MAXLINE];
typedef struct // URL 拆解器
{ 
    string host;
    string port;
    string path;
}url_t;

// 极简 Cache, 一把大锁, 随机替换, 锁内发送数据(不拷贝出去再发)
typedef struct // Cache 部分
{
    string url;
    char *data;
    int size;
    int valid;
} cache_file_t;
static cache_file_t cache_entries[MAX_CACHE_NUM];
static int total_cache_size = 0;
static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER; // ONE ONLY LOCK

int query_cache(string url, rio_t* client_rio)
{
    int client_fd = client_rio->rio_fd;
    pthread_mutex_lock(&cache_lock);
    for(int i=0;i<MAX_CACHE_NUM;i++) 
    {
        if(cache_entries[i].valid && strcmp(cache_entries[i].url, url)==0)
        {
            // 放 lock 外面 writen
            rio_writen(client_fd, cache_entries[i].data, cache_entries[i].size); 
            pthread_mutex_unlock(&cache_lock);
            return 1;
        }
    }
    pthread_mutex_unlock(&cache_lock);
    return 0;
}

int add_cache(string url, char *data, int size)
{
    pthread_mutex_lock(&cache_lock);
    for(int i=0;i<MAX_CACHE_NUM;i++)
    {
        if(cache_entries[i].valid && strcmp(cache_entries[i].url, url)==0)
        {
            pthread_mutex_unlock(&cache_lock);
            return 0; // 已存在，视为成功
        }
    }
    int count=0;
    for(int i=0;i<MAX_CACHE_NUM;i++)
        count+=(cache_entries[i].valid==0);
    if(!count)
    {
        int index=rand()%MAX_CACHE_NUM; //随机丢 index 位
        free(cache_entries[index].data);
        cache_entries[index].valid=0;
    }
    for(int i=0;i<MAX_CACHE_NUM;i++)
    {
        if(cache_entries[i].valid) continue;
        char *newdata = Malloc(size);
        memcpy(newdata, data, size);
        cache_entries[i].data = newdata;
        cache_entries[i].size = size;
        cache_entries[i].valid = 1;
        strcpy(cache_entries[i].url, url);
        break;
    }
    pthread_mutex_unlock(&cache_lock);
    return 0;
} 

int parse_url(string url, url_t* url_info)
{
    if(strncasecmp(url, "http://", 7))
    {
        fprintf(stderr, "NOOOOOOO http protocol: %s\n", url);
        return -1;
    }
    //eg. http://www.cmu.edu:8080/hub/index.html/ 
    char* host_start=url+7; // FROM http://w
    char* port_start=strchr(host_start, ':'); // FROM http://www.cmu.edu:
    char* path_start=strchr(host_start, '/'); // FROM http://www.cmu.edu:8080/

    // 无路径, 错误
    if(path_start == NULL) return -1;
    // 无端口号, 默认 80
    if(port_start==NULL) 
    {
        *path_start = '\0';
        strcpy(url_info->host, host_start);
        strcpy(url_info->port, "80");
        *path_start = '/';
        strcpy(url_info->path, path_start);
    }
    else // 有端口号
    {
        *port_start = '\0';
        *path_start = '\0';
        strcpy(url_info->host, host_start);
        strcpy(url_info->port, port_start+1);
        *path_start = '/';
        strcpy(url_info->path, path_start);
        *port_start = ':';
    }
    return 0;
}

int parse_header(rio_t* client_rio, string header_info, string host)
{
    // read many line until empty /r/n
    int has_host = 0;
    string buf;
    while(1)
    {   
        rio_readlineb(client_rio, buf, MAXLINE);
        if(strcmp(buf, "\r\n") == 0) break; // 结束行
        if(!strncasecmp(buf, "Host:", strlen("Host:")))
            has_host=1;
        if(!strncasecmp(buf, "Connection:",strlen("Connection:")) ||
            !strncasecmp(buf, "Proxy-Connection:", strlen("Proxy-Connection:")) ||
            !strncasecmp(buf, "User-Agent:", strlen("User-Agent:")))
            continue;
        strcat(header_info, buf);
    }
    if(!has_host)
    {
        sprintf(buf, "Host: %s\r\n", host);
        strcat(header_info, buf);
    }
    strcat(header_info, "Connection: close\r\n");
    strcat(header_info, "Proxy-Connection: close\r\n");
    strcat(header_info, user_agent_hdr);
    strcat(header_info, "\r\n");
    return 0;
}

void do_get(rio_t* client_rio, string url)
{
    url_t url_info;
    if(parse_url(url, &url_info)<0) 
    {
        fprintf(stderr, "Parse url error\n");
        return;
    }
    if(query_cache(url, client_rio)) return; // 检查 Cache
    string header_info = "";
    parse_header(client_rio, header_info, url_info.host);

    int server_fd = open_clientfd(url_info.host, url_info.port);
    if(server_fd<0) // 创建 TCP 网络连接, 返回套接字文件描述符, 没有就是炸了
    {
        fprintf(stderr, "Connect Fucked Up, Open connect to %s:%s error\n", url_info.host, url_info.port);
        close(server_fd); return;
    }

    rio_t server_rio;
    rio_readinitb(&server_rio, server_fd);
    string buf; // 准备请求行和请求头
    sprintf(buf, "GET %s HTTP/1.0\r\n%s", url_info.path, header_info);
    if(rio_writen(server_fd, buf, strlen(buf))!=strlen(buf))  // 发送请求行和请求头
    {
        fprintf(stderr, "Send request line and header Fucked Up\n");
        close(server_fd); return;
    }

    //拿着 server_rio 从服务器套接字读取数据
    int response_total = 0, response_current = 0;
    char file_cache[MAX_OBJECT_SIZE]; // 临时内存缓冲
    int client_fd = client_rio->rio_fd; // rio_readinitb 时候把 rio_fd 写入了
    while(response_current = rio_readnb(&server_rio, buf, MAXLINE)) // HTTP 响应可能分多次发送, 等 EOF
    {
        if(response_current<0) 
        {
            fprintf(stderr, "Read server response error\n");
            close(server_fd); return;
        }
        if(response_total + response_current < MAX_OBJECT_SIZE) // 上 Cache
            memcpy(file_cache + response_total, buf, response_current);
        response_total += response_current;
        if(rio_writen(client_fd, buf, response_current)!=response_current)
        {
            fprintf(stderr, "Send response to client error\n");
            close(server_fd); return;
        }
    }
    if(response_total < MAX_OBJECT_SIZE)
        add_cache(url, file_cache, response_total);
    close(server_fd);
    return;
}

void *thread(void *vargp)
{
    pthread_detach(pthread_self()); // 分离线程, 最终自刎归天
    int client_fd = *((int*)vargp); // 堆内存 -> 栈内存
    free(vargp);
    // 接下来处理 HTTP 1.0/1.1 
    // 一行 request line, N 行 Header
    // GET /path HTTP/1.1
    // Host: hostname
    // User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3
    // Connection: close
    // Proxy-Connection: close
    // 有点像 tiny.c 的 doit()?
    string buf;
    rio_t client_rio; // Robust IO? 解决 /r/n
    rio_readinitb(&client_rio, client_fd);
    if(rio_readlineb(&client_rio, buf, MAXLINE)<=0) // 读取客户端内容到 buf
    {
        fprintf(stderr,"Reading fucked up: %s\n",strerror(errno));
        close(client_fd); return NULL;
    }
    string method, url, http_version;
    //syntax belike: GET http://www.cmu.edu/hub/index.html HTTP/1.1
    if(sscanf(buf, "%s %s %s", method, url, http_version)!=3) 
    {
        fprintf(stderr,"Parsing fucked up: %s\n",strerror(errno));
        close(client_fd); return NULL;
    }
    if(!strcasecmp(method,"GET")) 
        do_get(&client_rio, url); // do get 资源!
    close(client_fd);
    return NULL;
}

int main(int argc, char** argv)
{
    // printf("%s", user_agent_hdr);
    Signal(SIGPIPE, SIG_IGN); //Proxylab.pdf, Page 10, Hint 4, no exit(0)

    int listenfd, *connfd;
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    pthread_t tid;

    if(argc!=2)
    {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    listenfd = Open_listenfd(argv[1]);
    while(1) // 循环接收客户端请求, 为每个连接请求创建一个线程, 再在这个线程中处理这个连接请求
    {
        clientlen = sizeof(struct sockaddr_storage);
        connfd = Malloc(sizeof(int));
        *connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        // WARNING: Accept 包装函数在遇到错误时会调用 unix_error, 从而使用 exit(0) 退出进程
        Pthread_create(&tid, NULL, thread, connfd);
    }
    close(listenfd);
    return 0;
}
