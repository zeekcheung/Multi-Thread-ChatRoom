#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <string>
#include <queue>

using namespace std;

#define SERVER_IP "127.0.0.1"  // 服务器IP 
#define SERVER_PORT 8888  // 服务器端口
#define BUFFER_SIZE 1024  // 缓冲区大小
#define NAME_LEN 20 // 用户名最大长度
#define MAX_CLIENT_NUM 3  // 客户端最大连接数

// 用户结构体，存储用户数据
typedef struct {
    int fd_id;  // 用户ID
    int socket;  // 用户套接字
    int online;  // 用户在线标识
    char username[NAME_LEN + 1];  // 用户名
} Client;

// 初始化用户数组
Client client[MAX_CLIENT_NUM] = {0}; 
// 定义消息队列数组：每个用户分配一个消息队列
queue<string> MsgQue[MAX_CLIENT_NUM];  
// 在线用户数
int online_clnt_num = 0;

// 聊天子线程
pthread_t chat_thread[MAX_CLIENT_NUM] = {0};
// 发送消息子线程
pthread_t send_thread[MAX_CLIENT_NUM] = {0};

// 在线用户数互斥锁
pthread_mutex_t num_mutex = PTHREAD_MUTEX_INITIALIZER;
// 消息队列互斥锁
pthread_mutex_t mutex[MAX_CLIENT_NUM] = {0};
// 消息队列信号量
pthread_cond_t cv[MAX_CLIENT_NUM] = {0};

// 发送消息函数
void *handle_send(void *data);
// 接收消息函数
void handle_recv(void *data);
// 处理消息的接收和转发
void *chat(void *data);

int main() {
    // 1. 创建套接字
    int server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_socket == 0) {
        perror("socket");
        return 1;
    }
    
    // 封装服务器地址信息
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(server_socket));
    addr.sin_family = AF_INET;  // 传输协议：IPv4
    addr.sin_addr.s_addr = inet_addr(SERVER_IP);  // 服务器IP：127.0.0.1
    addr.sin_port = htons(SERVER_PORT);  // 服务器端口：8888
    
    // 2. 绑定套接字
    if (bind(server_socket, (struct sockaddr *)&addr, sizeof(addr))) {
        perror("bind");
        return 2;
    }
    
    // 3. 启动监听
    if (listen(server_socket, MAX_CLIENT_NUM + 1)) {
        perror("listen");
        return 3;
    }
    printf("Server start successfully!\n");
    printf("Connect to the room by %s:%d\n\n",SERVER_IP,SERVER_PORT);
    
    // 4. 等待客户端请求
    while (1) {
        // 1）接收客户端请求，建立连接
        int client_sock = accept(server_socket, NULL, NULL);
        if (client_sock == -1) {
            perror("accept");
            return 4;
        }
        
        // 2）检查聊天室是否满人,发送连接信息
        string full_msg = "ERROR";
        string succ_msg = "OK";
        // 如果满人，发送错误信息，并断开连接
        if (online_clnt_num >= MAX_CLIENT_NUM) {   
            if (send(client_sock, full_msg.c_str(), full_msg.length(), 0)) {
                perror("send");
            }
            shutdown(client_sock, 2);
            continue;
        }
        // 否则，发送连接成功提示
        else {
            if (send(client_sock, succ_msg.c_str(), succ_msg.length(), 0)) {
                perror("send_func");
            }
        }
        
        // 3）接收客户端输入的用户名
        char username[NAME_LEN + 1] = {0};
        ssize_t state = recv(client_sock, username, NAME_LEN, 0);
        // 接受失败
        if (state < 0) {
            perror("recv");
            shutdown(client_sock, 2);
            continue;
        }
        // 用户离开，断开连接
        else if (state == 0) {
            shutdown(client_sock, 2);
            continue;
        }
        
        // 4）更新用户数组等信息，创建子线程
        for (int i = 0; i < MAX_CLIENT_NUM; i++) {
            if (!client[i].online) {
                // FIXME: 只有一个主线程，是否需要用户数的互斥锁？
                // 获取在线用户数的互斥锁
                pthread_mutex_lock(&num_mutex);
                
                // 设置用户信息
                client[i].fd_id = i;
                client[i].socket = client_sock;
                client[i].online = 1;
                memset(client[i].username, 0, sizeof(client[i].username));
                strcpy(client[i].username, username);
                
                // 创建用户消息队列的互斥锁和信号量
                mutex[i] = PTHREAD_MUTEX_INITIALIZER;
                cv[i] = PTHREAD_COND_INITIALIZER;
                
                online_clnt_num++;
                // 释放互斥锁
                pthread_mutex_unlock(&num_mutex);
                
                // 创建聊天子线程，通过参数传递用户信息
                // FIXME: 每个用户只用一个子线程读取和发送消息队列？
                pthread_create(&chat_thread[i], NULL, chat, (void *)&client[i]);
                printf("%s join in the chat room.\n", client[i].username);
                printf("Online User Number: %d\n",online_clnt_num);
                
                break;
            }
        }
    }
    
    // 5. 关闭套接字
    for (int i = 0; i < MAX_CLIENT_NUM; i++) {
        if (client[i].online) {
            shutdown(client[i].socket, 2);
        }
    }
    
    return 0;
}


// 发送消息函数
void *handle_send(void *data) {
    // 接收主线程传过来的用户信息
    Client *pipe = (Client *)data;
    
    while (1) {
        // 获取消息队列互斥锁
        pthread_mutex_lock(&mutex[pipe->fd_id]);
        
        // 如果用户消息队列为空，等待用户输入
        while (MsgQue[pipe->fd_id].empty()) {
            // 等待消息队列信号量
            pthread_cond_wait(&cv[pipe->fd_id],&mutex[pipe->fd_id]);
        }
        
        // 如果用户消息队列非空，读取消息并转发
        while (!MsgQue[pipe->fd_id].empty()) {
            // 读取消息队列中用户最先发送的信息
            string MsgBuff = MsgQue[pipe->fd_id].front();
            // 判断消息是否需要截断
            int n = MsgBuff.length();
            int trans_len = BUFFER_SIZE > n ? n : BUFFER_SIZE;
            // 如果消息超过缓冲区长度，则截断分批发送
            while (n > 0) {
                int len = send(pipe->socket, MsgBuff.c_str(), trans_len, 0);
                if (len < 0) {
                    perror("send");
                    return NULL;
                }
                n -= len;
                // 清除消息队列中已发送的消息
                MsgBuff.erase(0, len);
                trans_len = BUFFER_SIZE > n ? n : BUFFER_SIZE;
            }
            
            // 删除已经发送的信息
            MsgBuff.clear();
            MsgQue[pipe->fd_id].pop();
        }
        
        // 释放互斥锁
        pthread_mutex_unlock(&mutex[pipe->fd_id]);
    }
    
    return NULL;
}

// 接受消息函数
void handle_recv(void *data) {
    Client *pipe = (Client *)data;
    
    // 消息缓冲区
    string MsgBuff;
    int MsgLen = 0;
    
    // 单个传输缓冲区
    char buffer[BUFFER_SIZE + 1];
    int BuffLen = 0;
    
    // 接受信息
    while ((BuffLen= recv(pipe->socket, buffer, BUFFER_SIZE, 0)) > 0) {
        // 逐个字符进行读取，换行符作为终结符
        for (int i = 0; i < BuffLen; i++) {
            // 每条消息前拼上用户名
            if (MsgLen == 0) {
                char temp[100];
                sprintf(temp, "%s:", pipe->username);
                MsgBuff = temp;
                MsgLen = MsgBuff.length();
            }
            
            MsgBuff += buffer[i];
            MsgLen++;
            
            // 读取到换行符时，转发消息到其他用户的客户端
            if (buffer[i] == '\n') {
                for (int j = 0; j < MAX_CLIENT_NUM; j++) {
                    if (client[j].online && client[j].socket != pipe->socket) {
                        // 获取消息队列的互斥锁
                        pthread_mutex_lock(&mutex[j]);
                        // 将消息添加到消息队列中
                        MsgQue[j].push(MsgBuff);
                        // 释放信号量，通知子线程发送消息
                        pthread_cond_signal(&cv[j]);
                        // 释放互斥锁
                        pthread_mutex_unlock(&mutex[j]);
                    }
                }
                
                // 清空消息缓冲区，接受新消息
                MsgLen = 0;
                MsgBuff.clear();
            }
        }
        
        // 清空传输缓冲区
        BuffLen = 0;
        memset(buffer, 0, sizeof(buffer));
    }
    
    return;
}


// 处理消息的接收和转发
void *chat(void *data) {
    Client *pipe = (Client *)data;
    
    // 打印欢迎信息
    char hello[100];
    sprintf(hello, "Hello %s, Welcome to join the chat room.\nOnline User Number: %d\n", pipe->username, online_clnt_num);
    
    // 获取消息队列互斥锁
    pthread_mutex_lock(&mutex[pipe->fd_id]);
    // 将欢迎消息添加到消息队列中
    MsgQue[pipe->fd_id].push(hello);
    // 释放信号量
    pthread_cond_signal(&cv[pipe->fd_id]);
    // 释放互斥锁
    pthread_mutex_unlock(&mutex[pipe->fd_id]);
    
    memset(hello, 0, sizeof(hello));
    sprintf(hello, "New User %s join in!\nOnline User Number: %d\n", pipe->username, online_clnt_num);
    
    // 转发消息到其他用户的客户端
    for (int j = 0; j < MAX_CLIENT_NUM; j++) {
        if (client[j].online && client[j].socket != pipe->socket) {
            // 获取消息队列互斥锁
            pthread_mutex_lock(&mutex[j]);
            // 将欢迎消息添加到消息队列中
            MsgQue[j].push(hello);
            // 释放信号量
            pthread_cond_signal(&cv[j]);
            // 释放互斥锁
            pthread_mutex_unlock(&mutex[j]);
        }
    }
    
    // 创建当前套接字的转发线程 
    pthread_create(&send_thread[pipe->fd_id], NULL, handle_send, (void *)pipe);
    
    // 接收消息
    handle_recv(data);
    
    // 由于recv()函数是阻塞式的，如果handle_recv()函数返回了，说明该用户退出了聊天室
    pthread_mutex_lock(&num_mutex);
    pipe->online = 0;
    online_clnt_num--;
    pthread_mutex_unlock(&num_mutex);
    
    // 打印并转发用户离开信息
    printf("%s left the chat room.\nOnline Person Number: %d\n",pipe->username, online_clnt_num);
    char bye[100];
    sprintf(bye, "%s left the chat room.\nOnline Person Number: %d\n",pipe->username, online_clnt_num);
    
    for (int j = 0; j < MAX_CLIENT_NUM; j++) {
        if (client[j].online && client[j].socket != pipe->socket) {
            pthread_mutex_lock(&mutex[j]);
            MsgQue[j].push(bye);
            pthread_cond_signal(&cv[j]);
            pthread_mutex_unlock(&mutex[j]);
        }
    }
    
    // 销毁消息队列互斥锁和信号量，关闭发送消息的线程
    pthread_mutex_destroy(&mutex[pipe->fd_id]);
    pthread_cond_destroy(&cv[pipe->fd_id]);
    pthread_cancel(send_thread[pipe->fd_id]);
    
    return NULL;
}

 
